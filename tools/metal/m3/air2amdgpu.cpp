// air2amdgpu: one compute kernel of Apple AIR (the LLVM bitcode Metal compiles to, as metallibs carry it) as an AMDGPU code object for gfx1201
// (M3 of docs/metal-phase-plan.md, track b: AIR -> LLVM AMDGPU). Upstream LLVM reads AIR as it is (air64_v28, Metal 4, tested with LLVM 23):
// buffers are `ptr addrspace(1)` as on AMDGPU, and what makes a function a kernel is only metadata (!air.kernel, one record per argument).
//
//   air2amdgpu <kernel.air, .bc or .metallib> <kernel name> <out.hsaco>
//       prints `<name> <buffers> <write mask>` (bit i: buffer index i is written), for tools/metal/m3/build.sh's table
//   air2amdgpu --serve
//       the run-time translator: as root, a user client of the kext (include/rdna4mtlc.h) that translates each function the fake paravirtual
//       GPU's host asks for (the guest's metallib) and hands the code object back
//
// The code object holds an amdgpu_kernel of the same name whose kernargs are (the ABI the host fills, src/pvgpu.cpp):
//   +0           a 64-bit buffer address per Metal buffer index, 0 .. the highest index the kernel uses
//   +8 * N       u32 threads per threadgroup x, y, z
//   +8 * N + 12  u32 threads in the grid x, y, z (threads past it do nothing: dispatchThreads need not fill whole groups)
// It computes the AIR function's position arguments from the work-group and work-item ids and calls the AIR function, inlined.
// Supported so far: device and constant buffers, the arguments thread_position_in_grid, thread_position_in_threadgroup,
// threadgroup_position_in_grid, threads_per_threadgroup and threads_per_grid (uint, uint2 or uint3), thread_index_in_threadgroup and the
// SIMD-group indices (a SIMD group is a wave of 32), SIMD-group functions (simdOp() below), threadgroup memory and barriers, local
// arrays (as long as LLVM keeps them out of scratch memory, which the kext's runtime does not give kernels), device and threadgroup atomic
// read-modify-writes, and the math and integer functions of legalize() below. Anything else is reported and refused.
#include "lld/Common/Driver.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <IOKit/IOKitLib.h>
#include <array>
#include <map>
#include <string>
#include <unistd.h>

#include "../../../include/rdna4mtlc.h"

LLD_HAS_DRIVER(elf)

using namespace llvm;
using namespace llvm::PatternMatch;

static int fail(const std::string &why) {
	errs() << "air2amdgpu: " << why << "\n";
	return 1;
}

static uint64_t le(StringRef s, size_t at, size_t bytes) {
	uint64_t v = 0;
	for (size_t i = 0; i < bytes && at + i < s.size(); i++)
		v |= uint64_t(uint8_t(s[at + i])) << (8 * i);
	return v;
}

// The named function's bitcode in a metallib (layout measured, docs/m1-stream.md s.5b): header u64 function list offset at 0x18 and bitcode
// section offset at 0x48; the list is u32 count, then per function u32 bytes (counting itself) and tags {char[4], u16 length, data} to ENDT,
// among them NAME (the name, NUL-terminated), OFFT (u64 offsets: public metadata, private metadata, bitcode in its section) and MDSZ (u64
// bitcode bytes). Empty if it is not there or does not fit the file.
static StringRef functionBitcode(StringRef lib, StringRef name) {
	const uint64_t list = le(lib, 0x18, 8), section = le(lib, 0x48, 8);
	uint64_t at = list + 4;
	for (uint64_t f = 0, n = le(lib, list, 4); f < n && at + 4 <= lib.size(); f++) {
		const uint64_t end = at + le(lib, at, 4);
		std::string fname;
		uint64_t offset = ~0ull, bytes = 0;
		for (uint64_t t = at + 4; t + 6 <= end && t + 6 <= lib.size() && lib.substr(t, 4) != "ENDT"; t += 6 + le(lib, t + 4, 2)) {
			const StringRef tag = lib.substr(t, 4), data = lib.substr(t + 6, le(lib, t + 4, 2));
			if (tag == "NAME")
				fname = data.split('\0').first.str();
			else if (tag == "OFFT" && data.size() == 24)
				offset = le(data, 16, 8);
			else if (tag == "MDSZ" && data.size() == 8)
				bytes = le(data, 0, 8);
		}
		if (fname == name && offset != ~0ull && section + offset >= section && section + offset + bytes <= lib.size())
			return lib.substr(section + offset, bytes);
		if (end <= at)
			break;
		at = end;
	}
	return {};
}

static std::string mdString(const MDNode *n, unsigned i) {
	auto *s = i < n->getNumOperands() ? dyn_cast<MDString>(n->getOperand(i)) : nullptr;
	return s ? s->getString().str() : "";
}

static int64_t mdInt(const MDNode *n, unsigned i) {
	auto *c = i < n->getNumOperands() ? mdconst::dyn_extract<ConstantInt>(n->getOperand(i)) : nullptr;
	return c ? c->getSExtValue() : -1;
}

// One of the position arguments as the AIR function wants it (i32, <2 x i32> or <3 x i32>), from three i32 components.
static Value *asType(IRBuilder<> &b, Type *t, Value *const xyz[3]) {
	if (t->isIntegerTy(32))
		return xyz[0];
	auto *v = dyn_cast<FixedVectorType>(t);
	if (!v || !v->getElementType()->isIntegerTy(32) || v->getNumElements() > 3)
		return nullptr;
	Value *r = PoisonValue::get(t);
	for (unsigned k = 0; k < v->getNumElements(); k++)
		r = b.CreateInsertElement(r, xyz[k], k);
	return r;
}

// A Metal SIMD-group function (simd_<op>[.u|.s]) on a wave of 32, Metal's SIMD-group width; null if not supported. Measured on Apple's
// GPU: a shuffle up or down past the group's edge keeps the lane's own value; reductions, ballots and any/all see the active lanes only.
// Prefix sums shuffle upwards, so they are right when the inactive lanes are the top ones (a group cut by the grid's edge).
static Value *simdOp(IRBuilder<> &b, StringRef op, CallInst *c) {
	Type *t = c->getType();
	Value *x = c->arg_size() ? c->getArgOperand(0) : nullptr;
	Value *lane = b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {b.getInt32(-1), b.getInt32(0)});
	auto index = [&]() { return b.CreateZExtOrTrunc(c->getArgOperand(1), b.getInt32Ty()); };
	auto shuffle = [&](Value *v, Value *from) -> Value * {   // lane `from`'s v: 32-bit types directly, smaller ones through i32
		Type *vt = v->getType();
		const unsigned bits = vt->getPrimitiveSizeInBits();
		if (bits == 32)
			return b.CreateIntrinsic(Intrinsic::amdgcn_wave_shuffle, {vt}, {v, from});
		if (bits != 16 && bits != 8)
			return nullptr;
		Value *w = b.CreateZExt(b.CreateBitCast(v, b.getIntNTy(bits)), b.getInt32Ty());
		w = b.CreateIntrinsic(Intrinsic::amdgcn_wave_shuffle, {b.getInt32Ty()}, {w, from});
		return b.CreateBitCast(b.CreateTrunc(w, b.getIntNTy(bits)), vt);
	};
	auto ballot = [&](Value *cond) { return b.CreateIntrinsic(Intrinsic::amdgcn_ballot, {b.getInt32Ty()}, {cond}); };
	const auto [what, sign] = op.drop_front(5).split('.');   // "sum" and "u", "max" and "", ...
	const bool fp = t->isFloatingPointTy();
	if (what == "shuffle" || what == "broadcast")
		return shuffle(x, index());
	if (what == "shuffle_xor")
		return shuffle(x, b.CreateXor(lane, index()));
	if (what == "shuffle_down" || what == "shuffle_up") {
		Value *d = index();
		Value *from = what == "shuffle_down" ? b.CreateAdd(lane, d) : b.CreateSub(lane, d);
		Value *inside = what == "shuffle_down" ? b.CreateICmpULT(from, b.getInt32(32)) : b.CreateICmpUGE(lane, d);
		Value *v = shuffle(x, b.CreateAnd(from, 31));
		return v ? b.CreateSelect(inside, v, x) : nullptr;
	}
	if (what == "ballot")
		return b.CreateZExt(ballot(x), t);
	if (what == "any")
		return b.CreateICmpNE(ballot(x), b.getInt32(0));
	if (what == "all")
		return b.CreateICmpEQ(ballot(x), ballot(b.getTrue()));
	if (what == "prefix_inclusive_sum" || what == "prefix_exclusive_sum") {
		Value *sum = x;
		for (unsigned k = 1; k < 32; k <<= 1) {
			Value *below = shuffle(sum, b.CreateAnd(b.CreateSub(lane, b.getInt32(k)), 31));
			if (!below)
				return nullptr;
			Value *added = fp ? b.CreateFAdd(sum, below) : b.CreateAdd(sum, below);
			sum = b.CreateSelect(b.CreateICmpUGE(lane, b.getInt32(k)), added, sum);
		}
		if (what == "prefix_inclusive_sum")
			return sum;
		Value *before = shuffle(sum, b.CreateAnd(b.CreateSub(lane, b.getInt32(1)), 31));
		return b.CreateSelect(b.CreateICmpEQ(lane, b.getInt32(0)), Constant::getNullValue(t), before);
	}
	static const std::map<std::string, std::array<Intrinsic::ID, 3>> reduce = {   // float, signed, unsigned
		{"sum", {Intrinsic::amdgcn_wave_reduce_fadd, Intrinsic::amdgcn_wave_reduce_add, Intrinsic::amdgcn_wave_reduce_add}},
		{"max", {Intrinsic::amdgcn_wave_reduce_fmax, Intrinsic::amdgcn_wave_reduce_max, Intrinsic::amdgcn_wave_reduce_umax}},
		{"min", {Intrinsic::amdgcn_wave_reduce_fmin, Intrinsic::amdgcn_wave_reduce_min, Intrinsic::amdgcn_wave_reduce_umin}},
		{"and", {Intrinsic::not_intrinsic, Intrinsic::amdgcn_wave_reduce_and, Intrinsic::amdgcn_wave_reduce_and}},
		{"or", {Intrinsic::not_intrinsic, Intrinsic::amdgcn_wave_reduce_or, Intrinsic::amdgcn_wave_reduce_or}},
		{"xor", {Intrinsic::not_intrinsic, Intrinsic::amdgcn_wave_reduce_xor, Intrinsic::amdgcn_wave_reduce_xor}}};
	const auto it = reduce.find(what.str());
	if (it == reduce.end() || t->getPrimitiveSizeInBits() != 32)
		return nullptr;
	const Intrinsic::ID id = it->second[fp ? 0 : sign == "u" ? 2 : 1];
	return id == Intrinsic::not_intrinsic ? nullptr : b.CreateIntrinsic(id, {t}, {x, b.getInt32(0)});
}

// What AIR leaves in the inlined kernel that AMDGPU does not take, rewritten; the reason when something cannot be.
//  - AIR's constant address space (2) is AMDGPU's GDS: constant buffers become global memory (1) again.
//  - local arrays: AMDGPU's allocas live in address space 5 (private), seen through a flat pointer.
//  - air.* functions: LLVM intrinsics, fences and barriers, atomics.
// A Metal sampler as gfx12's sampler descriptor (S#, SQ_IMG_SAMP_WORD0-3), from AIR's packed constexpr sampler state (measured against the
// Metal compiler, 2026-10-22): address s [2:0], t [5:3], r [8:6] (0 zero/border, 1 edge, 2 repeat, 3 mirror), mag [9], min [11] linear, mip
// [14:13] (0 none, 1 nearest, 2 linear), pixel coordinates [15], anisotropy [23:19] = 2n - 1, min / max LOD halves at [39:24] / [55:40],
// border colour [57:56].
static uint32_t lod48(uint16_t half) {   // a half's value as gfx12's 4.8 fixed LOD, clamped to [0, 15.996]
	const int e = (half >> 10) & 31, m = half & 1023;
	if ((half & 0x8000) || !e)
		return 0;
	const double v = (1.0 + m / 1024.0) * double(1u << e) / 32768.0;
	return v >= 15.996 ? 0xfff : static_cast<uint32_t>(v * 256 + 0.5);
}
static Constant *airSampler(LLVMContext &ctx, uint64_t st) {
	static const uint32_t clamp[4] = {6, 2, 0, 1};   // zero -> CLAMP_BORDER, edge -> CLAMP_LAST_TEXEL, repeat -> WRAP, mirror -> MIRROR
	const uint32_t aniso = ((st >> 19) & 31) / 2 + 1, ratio = aniso >= 16 ? 4 : aniso >= 8 ? 3 : aniso >= 4 ? 2 : aniso >= 2 ? 1 : 0;
	const uint32_t w[4] = {
		clamp[st & 3] | clamp[(st >> 3) & 3] << 3 | clamp[(st >> 6) & 3] << 6 | ratio << 9 | uint32_t((st >> 15) & 1) << 15,
		lod48(uint16_t(st >> 24)) | lod48(uint16_t(st >> 40)) << 13,
		uint32_t((st >> 9) & 1) << 20 | uint32_t((st >> 11) & 1) << 22 | uint32_t((st >> 13) & 3) << 26,
		uint32_t((st >> 56) & 3) << 30};   // BORDER_COLOR_TYPE: 0 transparent black, 1 opaque black, 2 opaque white
	Type *i32 = Type::getInt32Ty(ctx);
	return ConstantVector::get({ConstantInt::get(i32, w[0]), ConstantInt::get(i32, w[1]), ConstantInt::get(i32, w[2]), ConstantInt::get(i32, w[3])});
}

static std::string legalize(Function &k) {
	LLVMContext &ctx = k.getContext();
	PointerType *global = PointerType::get(ctx, 1);
	SmallVector<Instruction *, 32> gone;
	// Constexpr samplers first: their state global (AIR's constant address space) becomes the S# constant of the call.
	std::map<CallInst *, Constant *> constSampler;
	for (Instruction &i : instructions(k))
		if (auto *c = dyn_cast<CallInst>(&i); c && c->getCalledFunction() && c->getCalledFunction()->getName().starts_with("air.sample_texture") &&
		                                        c->arg_size() > 1)
			if (auto *g = dyn_cast<GlobalVariable>(c->getArgOperand(1)->stripPointerCasts()); g && g->hasInitializer()) {
				// The state word: an i64, or the first of an array of them (both seen from the Metal compiler).
				auto *a = dyn_cast<ConstantDataArray>(g->getInitializer());
				auto *one = dyn_cast<ConstantInt>(g->getInitializer());
				if ((a && a->getNumElements() >= 1 && a->getElementType()->isIntegerTy(64)) || (one && one->getBitWidth() == 64)) {
					constSampler[c] = airSampler(ctx, a ? a->getElementAsInteger(0) : one->getZExtValue());
					c->setArgOperand(1, ConstantPointerNull::get(cast<PointerType>(c->getArgOperand(1)->getType())));
				}
			}
	for (Instruction &i : instructions(k)) {
		for (Use &u : i.operands())
			if (u->getType()->isPointerTy() && u->getType()->getPointerAddressSpace() == 2) {
				if (isa<ConstantPointerNull>(u.get()) || isa<UndefValue>(u.get()))
					u.set(isa<ConstantPointerNull>(u.get()) ? static_cast<Value *>(ConstantPointerNull::get(global)) : PoisonValue::get(global));
				else if (isa<Constant>(u.get()))
					return "data in AIR's constant address space (a constant table) is not supported yet";
			}
		if (i.getType()->isPointerTy() && i.getType()->getPointerAddressSpace() == 2)
			i.mutateType(global);
	}
	for (Instruction &i : instructions(k))
		if (auto *c = dyn_cast<AddrSpaceCastInst>(&i); c && c->getSrcAddressSpace() == c->getDestAddressSpace()) {
			c->replaceAllUsesWith(c->getOperand(0));
			gone.push_back(c);
		} else if (auto *m = dyn_cast<MemTransferInst>(&i); m && m->getRawSource()->getType() != m->getCalledFunction()->getArg(1)->getType()) {
			IRBuilder<> b(m);
			b.CreateMemCpy(m->getRawDest(), m->getDestAlign(), m->getRawSource(), m->getSourceAlign(), m->getLength(), m->isVolatile());
			gone.push_back(m);
		} else if (auto *a = dyn_cast<AllocaInst>(&i); a && a->getAddressSpace() != 5) {
			IRBuilder<> b(a);
			auto *priv = b.CreateAlloca(a->getAllocatedType(), 5, a->getArraySize());
			priv->setAlignment(a->getAlign());
			a->replaceAllUsesWith(b.CreateAddrSpaceCast(priv, a->getType()));
			gone.push_back(a);
		} else if (auto *l = dyn_cast<LifetimeIntrinsic>(&i)) {
			gone.push_back(l);   // they must name an alloca directly; the ones moved above are cast
		}
	for (Instruction *i : gone)
		i->eraseFromParent();
	gone.clear();

	// air.[fast_]<function>.<type>: the same function as an LLVM intrinsic of the call's type, or rewritten below.
	static const std::map<std::string, Intrinsic::ID> same = {
		{"sqrt", Intrinsic::sqrt}, {"exp", Intrinsic::exp}, {"exp2", Intrinsic::exp2}, {"log", Intrinsic::log}, {"log2", Intrinsic::log2},
		{"sin", Intrinsic::sin}, {"cos", Intrinsic::cos}, {"fabs", Intrinsic::fabs}, {"floor", Intrinsic::floor}, {"ceil", Intrinsic::ceil},
		{"trunc", Intrinsic::trunc}, {"rint", Intrinsic::rint}, {"round", Intrinsic::round}, {"fmax", Intrinsic::maxnum},
		{"fmin", Intrinsic::minnum}, {"fma", Intrinsic::fma}, {"popcount", Intrinsic::ctpop}, {"min.s", Intrinsic::smin},
		{"min.u", Intrinsic::umin}, {"max.s", Intrinsic::smax}, {"max.u", Intrinsic::umax}};
	static const std::map<std::string, std::pair<Intrinsic::ID, Intrinsic::ID>> clampOps = {   // clamp: its max and min
		{"clamp", {Intrinsic::maxnum, Intrinsic::minnum}}, {"clamp.s", {Intrinsic::smax, Intrinsic::smin}}, {"clamp.u", {Intrinsic::umax, Intrinsic::umin}}};
	static const std::map<std::string, AtomicRMWInst::BinOp> rmw = {
		{"add", AtomicRMWInst::Add}, {"sub", AtomicRMWInst::Sub}, {"and", AtomicRMWInst::And}, {"or", AtomicRMWInst::Or},
		{"xor", AtomicRMWInst::Xor}, {"xchg", AtomicRMWInst::Xchg}, {"min.s", AtomicRMWInst::Min}, {"max.s", AtomicRMWInst::Max},
		{"min.u", AtomicRMWInst::UMin}, {"max.u", AtomicRMWInst::UMax}};
	for (Instruction &i : instructions(k)) {
		auto *c = dyn_cast<CallInst>(&i);
		Function *f = c ? c->getCalledFunction() : nullptr;
		if (!f || !f->getName().starts_with("air."))
			continue;
		StringRef name = f->getName().drop_front(4);
		name.consume_front("fast_");
		IRBuilder<> b(c);
		Value *r = nullptr;
		const auto [op, type] = name.rsplit('.');   // e.g. "min.s" and "i32", "sqrt" and "f32", "wg" and "barrier"
		// A descriptor (T# 8 dwords, S# 4) at the handle's address, through the constant address space so it lands in SGPRs.
		auto descriptor = [&](Value *handle, unsigned dwords) {
			Value *at = b.CreateIntToPtr(b.CreatePtrToInt(handle, b.getInt64Ty()), PointerType::get(ctx, 4));
			return b.CreateAlignedLoad(FixedVectorType::get(b.getInt32Ty(), dwords), at, Align(16));
		};
		if (name.starts_with("sample_texture_2d.v4f32") || name.starts_with("read_texture_2d.v4f32")) {
			// sample (texture, sampler, float2 coord, i1, int2 offset, i1 lod or bias, float, float, i32) / read (texture, sampler, uint2, int2, lod, i32)
			const bool sample = name.starts_with("sample");
			if (c->arg_size() < 5 || !isa<ConstantAggregateZero>(c->getArgOperand(sample ? 4 : 3)) ||
			    (sample && c->arg_size() > 5 && !match(c->getArgOperand(5), m_Zero())) || (!sample && !match(c->getArgOperand(4), m_Zero())))
				return f->getName().str() + " with an offset, a LOD or a bias is not supported yet";
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(2);
			Value *x = b.CreateExtractElement(xy, uint64_t(0)), *y = b.CreateExtractElement(xy, 1);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel;
			if (sample) {
				Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_2d, {v4f, b.getFloatTy(), t->getType(), sm->getType()},   // descriptors overloaded
				                          {b.getInt32(15), x, y, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			} else {
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2d, {v4f, b.getInt32Ty(), t->getType()}, {b.getInt32(15), x, y, t, b.getInt32(0), b.getInt32(0)});
			}
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (name == "discard_fragment") {   // fragment only: the lane ends
			b.CreateIntrinsic(Intrinsic::amdgcn_kill, {}, {b.getFalse()});
		} else if (name == "get_read_sampler") {
			r = PoisonValue::get(c->getType());   // read_texture's sampler operand: unused
		} else if (op.starts_with("simd_")) {
			r = simdOp(b, op, c);
			if (!r)
				return f->getName().str() + " is not supported yet";
		} else if (name == "wg.barrier") {
			// (memory flags: 1 device, 2 threadgroup, 4 texture; scope): fence, the hardware barrier, fence.
			const SyncScope::ID scope = ctx.getOrInsertSyncScopeID(cast<ConstantInt>(c->getArgOperand(0))->getZExtValue() & 1 ? "agent" : "workgroup");
			b.CreateFence(AtomicOrdering::Release, scope);
			b.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {});
			b.CreateFence(AtomicOrdering::Acquire, scope);
		} else if (name.starts_with("convert.")) {   // convert.<to kind>.<to type>.<from kind>.<from type>; kinds f, s, u
			SmallVector<StringRef, 5> k;
			name.split(k, '.');
			if (k.size() != 5)
				return f->getName().str() + " is not supported yet";
			Value *v = c->getArgOperand(0);
			Type *to = c->getType();
			if (k[1] == "f" && k[3] == "f")
				r = b.CreateFPCast(v, to);
			else if (k[3] == "f")   // float to integer: saturating
				r = b.CreateIntrinsic(k[1] == "s" ? Intrinsic::fptosi_sat : Intrinsic::fptoui_sat, {to, v->getType()}, {v});
			else if (k[1] == "f")
				r = k[3] == "s" ? b.CreateSIToFP(v, to) : b.CreateUIToFP(v, to);
			else
				r = b.CreateIntCast(v, to, k[3] == "s");
		} else if (name.consume_front("atomic.global.") || name.consume_front("atomic.local.")) {
			// (pointer, value, memory order, scope, volatile): Metal's are relaxed; device or threadgroup scope by the memory.
			auto it = rmw.find(name.rsplit('.').first.str());   // "add.u" -> "add"/"min.u"
			if (it == rmw.end())
				it = rmw.find(name.rsplit('.').first.rsplit('.').first.str());
			if (it == rmw.end())
				return f->getName().str() + " is not supported yet";
			const bool local = f->getName().contains(".local.");
			r = b.CreateAtomicRMW(it->second, c->getArgOperand(0), c->getArgOperand(1), MaybeAlign(), AtomicOrdering::Monotonic,
			                      ctx.getOrInsertSyncScopeID(local ? "workgroup" : "agent"));
		} else if (same.count(op.str())) {
			SmallVector<Value *, 3> args;   // one to three operands of the call's type; AIR's i1 flags have no counterpart
			for (Value *v : c->args())
				if (!v->getType()->isIntegerTy(1))
					args.push_back(v);
			r = b.CreateIntrinsic(same.at(op.str()), {c->getType()}, args);
		} else if (op == "clz" || op == "ctz") {   // (x, is zero poison)
			r = b.CreateIntrinsic(op == "clz" ? Intrinsic::ctlz : Intrinsic::cttz, {c->getType()}, {c->getArgOperand(0), b.getFalse()});
		} else if (name.starts_with("get_width_texture_2d") || name.starts_with("get_height_texture_2d")) {
			// (texture, lod): from the T# the host wrote (src/pvgpu.cpp): width - 1 in word1 [31:30] and word2 [13:0] << 2, height - 1 in
			// word2 [27:14]; then >> lod, at least 1.
			Value *t = descriptor(c->getArgOperand(0), 8), *w1 = b.CreateExtractElement(t, 1), *w2 = b.CreateExtractElement(t, 2);
			Value *size = name.starts_with("get_width") ? b.CreateOr(b.CreateLShr(w1, 30), b.CreateShl(b.CreateAnd(w2, 0x3fff), 2))
			                                            : b.CreateAnd(b.CreateLShr(w2, 14), 0x3fff);
			r = b.CreateIntrinsic(Intrinsic::umax, {b.getInt32Ty()}, {b.CreateLShr(b.CreateAdd(size, b.getInt32(1)), c->getArgOperand(1)), b.getInt32(1)});
		} else if (auto cl = clampOps.find(op.str()); cl != clampOps.end()) {   // (x, low, high): min(max(x, low), high)
			Value *lo = b.CreateIntrinsic(cl->second.first, {c->getType()}, {c->getArgOperand(0), c->getArgOperand(1)});
			r = b.CreateIntrinsic(cl->second.second, {c->getType()}, {lo, c->getArgOperand(2)});
		} else if (op == "mix") {   // (x, y, a): x + (y - x) * a
			r = b.CreateFAdd(c->getArgOperand(0), b.CreateFMul(b.CreateFSub(c->getArgOperand(1), c->getArgOperand(0)), c->getArgOperand(2)));
		} else if (op == "dot") {   // (a, b) of a float vector type: the sum of the products
			Value *p = b.CreateFMul(c->getArgOperand(0), c->getArgOperand(1));
			r = b.CreateExtractElement(p, uint64_t(0));
			for (unsigned k = 1; k < cast<FixedVectorType>(p->getType())->getNumElements(); k++)
				r = b.CreateFAdd(r, b.CreateExtractElement(p, k));
		} else if (op == "fmod") {   // C's fmod, as Metal's: x - y * trunc(x / y)
			r = b.CreateFRem(c->getArgOperand(0), c->getArgOperand(1));
		} else if (op == "rsqrt") {
			r = b.CreateFDiv(ConstantFP::get(c->getType(), 1.0), b.CreateIntrinsic(Intrinsic::sqrt, {c->getType()}, {c->getArgOperand(0)}));
		} else if (op == "pow" || op == "powr") {   // fast math's x^y = 2^(y log2 x)
			r = b.CreateIntrinsic(Intrinsic::exp2, {c->getType()},
			                      {b.CreateFMul(c->getArgOperand(1), b.CreateIntrinsic(Intrinsic::log2, {c->getType()}, {c->getArgOperand(0)}))});
		} else if (op == "abs.s") {
			r = b.CreateIntrinsic(Intrinsic::abs, {c->getType()}, {c->getArgOperand(0), b.getFalse()});
		} else {
			return f->getName().str() + " is not supported yet";
		}
		if (r)
			c->replaceAllUsesWith(r);
		gone.push_back(c);
	}
	for (Instruction *i : gone)
		i->eraseFromParent();
	return "";
}

// The entry (a kernel or shader wrapping the AIR function `air`) as an object for gfx1201: the AIR function inlined, legalised, optimised.
static std::string compile(Module &m, Function *entry, Function *air, bool pal, SmallVectorImpl<char> &object) {
	// The AIR function and whatever it calls inlined into the entry; the module's other entry points go (they need not translate).
	for (const char *list : {"air.kernel", "air.vertex", "air.fragment"})
		for (unsigned i = 0; m.getNamedMetadata(list) && i < m.getNamedMetadata(list)->getNumOperands(); i++)
			if (auto *k = mdconst::dyn_extract_or_null<Function>(m.getNamedMetadata(list)->getOperand(i)->getOperand(0)); k && k != air) {
				k->replaceAllUsesWith(PoisonValue::get(k->getType()));
				k->eraseFromParent();
			}
	for (bool again = true; again;) {
		again = false;
		for (Instruction &i : instructions(*entry))
			if (auto *c = dyn_cast<CallInst>(&i); c && c->getCalledFunction() && !c->getCalledFunction()->isDeclaration()) {
				InlineFunctionInfo ifi;
				if (!InlineFunction(*c, ifi).isSuccess())
					return "cannot inline " + c->getCalledFunction()->getName().str();
				again = true;
				break;
			}
	}
	// Inlined functions go (the AIR function too): their original bodies would only have to be legalised as well.
	for (bool again = true; again;) {
		again = false;
		for (Function &f : make_early_inc_range(m))
			if (&f != entry && !f.isDeclaration() && f.use_empty()) {
				f.eraseFromParent();
				again = true;
			}
	}
	if (std::string why = legalize(*entry); !why.empty())
		return why;

	// Retarget: AMDGPU's triple (HSA for kernels, PAL for shaders: its metadata carries their register values) and data layout, no AIR
	// metadata.
	const Triple triple(pal ? "amdgcn-amd-amdpal" : "amdgcn-amd-amdhsa");
	std::string err;
	const Target *target = TargetRegistry::lookupTarget(triple, err);
	if (!target)
		return "no AMDGPU target in this LLVM: " + err;
	std::unique_ptr<TargetMachine> tm(target->createTargetMachine(triple, "gfx1201", "", TargetOptions(), std::nullopt));
	m.setTargetTriple(triple);
	m.setDataLayout(tm->createDataLayout());
	SmallVector<NamedMDNode *, 8> airMd;
	for (NamedMDNode &n : m.named_metadata())
		if (n.getName().starts_with("air."))
			airMd.push_back(&n);
	for (NamedMDNode *n : airMd)
		m.eraseNamedMetadata(n);
	if (verifyModule(m, &errs()))
		return "the retargeted module does not verify";

	// Only the new kernel stays visible: the module's other AIR kernels and functions go unless it uses them.
	for (Function &f : m)
		if (&f != entry && !f.isDeclaration())
			f.setLinkage(GlobalValue::InternalLinkage);

	// Optimise (inlines the AIR function), then refuse what AMDGPU cannot take as AIR leaves it.
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cam;
	ModuleAnalysisManager mam;
	PassBuilder pb(tm.get());
	pb.registerModuleAnalyses(mam);
	pb.registerCGSCCAnalyses(cam);
	pb.registerFunctionAnalyses(fam);
	pb.registerLoopAnalyses(lam);
	pb.crossRegisterProxies(lam, fam, cam, mam);
	pb.buildPerModuleDefaultPipeline(OptimizationLevel::O2).run(m, mam);
	for (Instruction &i : instructions(*entry))
		if (isa<AllocaInst>(i))
			return "private memory (an alloca in AIR's address space 0) is not supported yet";

	raw_svector_ostream objectOut(object);
	legacy::PassManager cg;
	if (tm->addPassesToEmitFile(cg, objectOut, nullptr, CodeGenFileType::ObjectFile))
		return "this LLVM cannot emit an AMDGPU object";
	cg.run(m);
	return "";
}

static bool gLldSpent;   // lld said it cannot be run again in this process: --serve exits after its reply (launchd starts a new one)

struct Kernel {
	std::string hsaco;   // the linked code object
	unsigned buffers;
	uint32_t written;
};

// A vertex or fragment function as a gfx12 shader with the hardware ABI of the kext's hand-written triangle shaders, for the host's draws
// (src/pvgpu.cpp), as an RGFX blob: {char[4] "RGFX", u32 stage (1 vertex, 2 fragment), u32 code bytes, u32 SPI_SHADER_PGM_RSRC1, u32 RSRC2,
// u32 SPI_PS_INPUT_ENA, u32 SPI_PS_INPUT_ADDR, u32 attributes}, then the code (relocation-free, the entry at its start), then per attribute
// {u32 hash of its AIR name (FNV-1a), u32 flags: bit 0 flat}: the vertex function's outputs in ring-slot order, the fragment function's
// inputs in hardware-input order; the host matches them by name (SPI_PS_INPUT_CNTL_n).
// Both stages get two user SGPRs, the address of the stage's table (the host's): 64-bit buffer addresses by Metal buffer index at +0, and in
// the fragment stage the textures' T#s (8 dwords) at +64 + 32 n and the samplers' S#s (4 dwords) at +320 + 16 n.
//   vertex   amdgpu_gs, an NGG shader as src/shaders/nggcol.s: s3 = merged_wave_info ([7:0] vertices, [15:8] primitives, [27:24] wave in
//            the subgroup), s5 = gs_attr_offset (s0-s7 are the merged stage's system SGPRs), s8-s9 the table, s10 the first vertex
//            (added to VertexID, as RADV's base vertex), s11 the instance id, v0 = the packed primitive, v3 = VertexID; the position goes out as pos0, the primitive
//            as prim, the other outputs to the attribute ring, output n in slot n (buffer_store_b128 at index = thread in the subgroup,
//            offset 16 n, soffset = gs_attr_offset << 9, scope device; the ring's descriptor from the literals 0xc01a0001-3, which the
//            host patches, as nggcol.s and Mesa's ac_nir_store_parameters_to_attr_ring). So far: vertex_id, instance_id, buffers.
//   fragment amdgpu_ps exporting render target 0 as four floats (SPI_SHADER_COL_FORMAT 32_ABGR), s0-s1 the table, PrimMask in s2;
//            buffers, textures, samplers, the position and inputs, input n from hardware input n: interpolated at the centre (centroid
//            too, single-sample targets only so far), with or without perspective (ds_param_load + v_interp_p10/p2, as pscol.s), or flat (ds_param_load and quad lane 0,
//            the provoking vertex's value, as ACO's emit_interp_mov_instr).
// A value between the stages is up to four 32-bit words: float, half, int, short, char or bool, scalar or vector (half widened to float,
// smaller integers zero-extended).
static std::string translateShader(Module &m, Function *air, const MDNode *md, bool vertex, Kernel &out) {
	LLVMContext &ctx = m.getContext();
	Type *i32 = Type::getInt32Ty(ctx), *f32 = Type::getFloatTy(ctx);
	const auto *outputs = md->getNumOperands() > 1 ? dyn_cast<MDNode>(md->getOperand(1)) : nullptr;
	const auto *inputs = md->getNumOperands() > 2 ? dyn_cast<MDNode>(md->getOperand(2)) : nullptr;
	if (!outputs || !inputs)
		return "the AIR shader's metadata is not as expected";
	// The inputs: index -> where its value comes from (-1: unused, poison).
	// 0 vertex id, 1 instance id, 3 the fragment's position, 100 + n buffer n, 200 + n texture n, 300 + n sampler n, 400 + n input n
	std::map<unsigned, int> from;
	struct Var { unsigned index; uint32_t hash; bool flat; unsigned bary; };   // a value between the stages (index: its argument or member)
	std::vector<Var> vars;
	auto fnv = [](const std::string &name) {
		uint32_t h = 2166136261u;
		for (unsigned char c : name)
			h = (h ^ c) * 16777619u;
		return h;
	};
	for (const MDOperand &op : inputs->operands()) {
		const auto *a = cast<MDNode>(op);
		const unsigned index = static_cast<unsigned>(mdInt(a, 0));
		const std::string kind = mdString(a, 1);
		bool unused = false;
		for (unsigned k = 2; k < a->getNumOperands(); k++)
			unused |= mdString(a, k) == "air.arg_unused";
		if (vertex && kind == "air.vertex_id")
			from[index] = 0;
		else if (vertex && kind == "air.instance_id")
			from[index] = 1;
		else if (!vertex && kind == "air.fragment_input" && !unused) {
			// The interpolation: air.flat, or air.center / air.centroid with air.perspective / air.no_perspective; the hardware's
			// barycentric inputs PERSP_CENTER 4 and LINEAR_CENTER 8 (the arguments below).
			bool flat = false, centroid = false, linear = false, center = false;   // centroid: accepted, read at the centre (below)
			for (unsigned k = 3; k < a->getNumOperands(); k++) {
				const std::string q = mdString(a, k);
				flat |= q == "air.flat";
				centroid |= q == "air.centroid";
				center |= q == "air.center";
				linear |= q == "air.no_perspective";
			}
			if (!flat && !center && !centroid)
				return "fragment input " + std::to_string(index) + ": only center, centroid or flat interpolation so far";
			// ponytail: centroid read at the centre, exact for the single-sample targets drawn so far (the card's PERSP_CENTROID is
			// wrong next to PERSP_CENTER, mtlprobe vycase 8, 2026-10-22; radeonsi also forces the centre without MSAA); MSAA needs it back.
			from[index] = 400 + static_cast<int>(vars.size());
			vars.push_back({index, fnv(mdString(a, 2)), flat, linear ? 8u : 4u});
		}
		else if (!vertex && kind == "air.position" && !unused)
			from[index] = 3;
		else if (kind == "air.buffer" && !unused) {
			int location = -1;
			for (unsigned k = 2; k + 1 < a->getNumOperands(); k++)
				if (mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
			if (location < 0 || location > 7 || index >= air->arg_size() ||
			    (air->getArg(index)->getType() != PointerType::get(ctx, 1) && air->getArg(index)->getType() != PointerType::get(ctx, 2)))
				return "buffer argument " + std::to_string(index) + ": only device and constant buffers at indices 0-7 so far";
			from[index] = 100 + location;
		} else if (!vertex && (kind == "air.texture" || kind == "air.sampler") && !unused) {
			int location = -1;
			for (unsigned k = 2; k + 1 < a->getNumOperands(); k++)
				if (mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
			if (location < 0 || location > 7)
				return kind + " argument " + std::to_string(index) + ": only indices 0-7 so far";
			from[index] = (kind == "air.texture" ? 200 : 300) + location;
		}
		else if (unused)
			from[index] = -1;
		else
			return std::string(vertex ? "vertex" : "fragment") + " input " + kind + " is not supported yet";
	}
	if (from.size() != air->arg_size())
		return "not every argument of the AIR shader is described";
	int position = -1;
	for (unsigned k = 0; k < outputs->getNumOperands(); k++) {
		const auto *o = cast<MDNode>(outputs->getOperand(k));
		const std::string kind = mdString(o, 0);
		if (vertex && kind == "air.position")
			position = static_cast<int>(k);
		else if (vertex && kind == "air.vertex_output")
			vars.push_back({k, fnv(mdString(o, 1)), false, 0});
		else if (!(!vertex && kind == "air.render_target" && outputs->getNumOperands() == 1 && air->getReturnType() == FixedVectorType::get(f32, 4)))
			return std::string(vertex ? "vertex" : "fragment") + " output " + kind + " is not supported yet";
	}
	if (vertex && position < 0)
		return "the vertex function has no position";
	if (vars.size() > 16)
		return "more than 16 values between the stages (so far)";
	// A value's type: up to four floats, halves or integers of up to 32 bits (bool included); integers only flat.
	for (const Var &v : vars) {
		Type *t = vertex ? cast<StructType>(air->getReturnType())->getElementType(v.index) : air->getArg(v.index)->getType();
		auto *vt = dyn_cast<FixedVectorType>(t);
		Type *e = vt ? vt->getElementType() : t;
		if ((vt && vt->getNumElements() > 4) || !(e == f32 || e->isHalfTy() || (e->isIntegerTy() && e->getIntegerBitWidth() <= 32)))
			return "a value between the stages is not up to four floats, halves or 32-bit integers (so far)";
		if (!vertex && e->isIntegerTy() && !v.flat)
			return "an integer fragment input that is not flat";
	}

	// The shader.
	// vertex: s0-s5, v0-v3; fragment: PrimMask (s0), then the hardware's pixel inputs in their order up to the position (one argument each:
	// PERSP_SAMPLE, PERSP_CENTER, PERSP_CENTROID, PERSP_PULL_MODEL, LINEAR_SAMPLE, LINEAR_CENTER, LINEAR_CENTROID, LINE_STIPPLE, POS_X, POS_Y,
	// POS_Z, POS_W); LLVM enables the ones used and reports them (SPI_PS_INPUT_ENA/ADDR).
	Type *v2f32 = FixedVectorType::get(f32, 2), *v3f32 = FixedVectorType::get(f32, 3), *v4f32 = FixedVectorType::get(f32, 4);
	SmallVector<Type *, 16> params = vertex ? SmallVector<Type *, 16>(16, i32)   // s0-s7, s8-s11, v0-v3
	                                        : SmallVector<Type *, 16>{i32, i32, i32, v2f32, v2f32, v2f32, v3f32, v2f32, v2f32, v2f32, f32, f32, f32, f32, f32};
	// The arguments by name: vertex s3, s5, s8-s9, s10, s11, v0, v3; fragment s0-s1, s2, POS_X (the barycentrics: Var::bary).
	const unsigned kInfo = 3, kAttrOffset = 5, kFirstVertex = 10, kInstance = 11, kPrim = 12, kVertexId = 15, kPrimMask = 2, kPosX = 11;
	const unsigned kTable = vertex ? 8 : 0;
	Function *f = Function::Create(FunctionType::get(Type::getVoidTy(ctx), params, false), GlobalValue::ExternalLinkage, vertex ? "vs" : "ps", &m);
	f->setCallingConv(vertex ? CallingConv::AMDGPU_GS : CallingConv::AMDGPU_PS);
	for (unsigned k = 0; k < (vertex ? 12u : 3u); k++)
		f->addParamAttr(k, Attribute::InReg);
	IRBuilder<> b(BasicBlock::Create(ctx, "entry", f));
	// A value as four floats' bits (missing components 0, w 1 as Metal's vertex outputs have none) and back; for float4 the identity.
	auto components = [](Type *t) { auto *vt = dyn_cast<FixedVectorType>(t); return vt ? vt->getNumElements() : 1u; };
	auto toVec4 = [&](Value *v) -> Value * {
		Value *r = ConstantVector::get({ConstantFP::get(f32, 0.0), ConstantFP::get(f32, 0.0), ConstantFP::get(f32, 0.0), ConstantFP::get(f32, 1.0)});
		const bool vec = v->getType()->isVectorTy();
		for (unsigned k = 0; k < components(v->getType()); k++) {
			Value *x = vec ? b.CreateExtractElement(v, k) : v;
			if (x->getType()->isHalfTy())
				x = b.CreateFPExt(x, f32);
			else if (x->getType()->isIntegerTy())
				x = b.CreateBitCast(b.CreateZExt(x, i32), f32);
			r = b.CreateInsertElement(r, x, k);
		}
		return r;
	};
	auto fromVec4 = [&](Value *v4, Type *t) -> Value * {
		auto *vt = dyn_cast<FixedVectorType>(t);
		Type *e = vt ? vt->getElementType() : t;
		Value *r = vt ? PoisonValue::get(t) : nullptr;
		for (unsigned k = 0; k < components(t); k++) {
			Value *x = b.CreateExtractElement(v4, k);
			if (e->isHalfTy())
				x = b.CreateFPTrunc(x, e);
			else if (e->isIntegerTy())
				x = b.CreateTrunc(b.CreateBitCast(x, i32), e);
			r = vt ? b.CreateInsertElement(r, x, k) : x;
		}
		return r;
	};
	std::vector<Value *> inValues;   // fragment: input n's value as four floats' bits
	auto table = [&]() {   // the stage's table address, from its two user SGPRs
		return b.CreateOr(b.CreateZExt(f->getArg(kTable), b.getInt64Ty()), b.CreateShl(b.CreateZExt(f->getArg(kTable + 1), b.getInt64Ty()), 32));
	};
	auto buffer = [&](unsigned n, Type *t) -> Value * {   // buffer n's address from the stage's table (constant memory, scalar loads)
		Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(8 * n)), PointerType::get(ctx, 4));
		Value *address = b.CreateIntToPtr(b.CreateAlignedLoad(b.getInt64Ty(), at, Align(8)), PointerType::get(ctx, 1));
		return t == address->getType() ? address : b.CreateAddrSpaceCast(address, t);   // a constant buffer: made global again by legalize()
	};
	// A texture or sampler handle: the address of its T# (table + 64 + 32 n) or S# (table + 320 + 16 n), which legalize() loads.
	auto handle = [&](uint64_t offset, Type *t) -> Value * { return b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(offset)), t); };
	auto fragPosition = [&]() {        // fragment: the pixel's window position x, y, depth z and w (POS_X..W_FLOAT)
		Value *r = PoisonValue::get(v4f32);
		for (unsigned c = 0; c < 4; c++)
			r = b.CreateInsertElement(r, f->getArg(kPosX + c), c);
		return r;
	};
	auto callAir = [&](Value *vid) {
		SmallVector<Value *, 4> args;
		for (unsigned k = 0; k < air->arg_size(); k++)
			args.push_back(from[k] == 0 ? vid
			               : from[k] == 1 ? f->getArg(kInstance)
			               : from[k] >= 400 ? fromVec4(inValues[static_cast<unsigned>(from[k] - 400)], air->getArg(k)->getType())
			               : from[k] == 3 ? fromVec4(fragPosition(), air->getArg(k)->getType())
			               : from[k] >= 300 ? handle(320 + 16 * static_cast<unsigned>(from[k] - 300), air->getArg(k)->getType())
			               : from[k] >= 200 ? handle(64 + 32 * static_cast<unsigned>(from[k] - 200), air->getArg(k)->getType())
			               : from[k] >= 100 ? buffer(static_cast<unsigned>(from[k] - 100), air->getArg(k)->getType())
			                              : PoisonValue::get(air->getArg(k)->getType()));
		return b.CreateCall(air, args);
	};
	auto exportF32 = [&](unsigned target, Value *v4, bool vm) {
		Value *c[4];
		for (unsigned k = 0; k < 4; k++)
			c[k] = b.CreateExtractElement(v4, k);
		b.CreateIntrinsic(Intrinsic::amdgcn_exp, {f32}, {b.getInt32(target), b.getInt32(15), c[0], c[1], c[2], c[3], b.getTrue(), b.getInt1(vm)});
	};
	if (vertex) {
		b.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, {b.getInt64(-1)});
		Value *lane = b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {b.getInt32(-1), b.getInt32(0)});
		Value *info = f->getArg(kInfo), *vertices = b.CreateAnd(info, 255);
		BasicBlock *entry = b.GetInsertBlock(), *pos = BasicBlock::Create(ctx, "pos", f), *afterPos = BasicBlock::Create(ctx, "after_pos", f);
		BasicBlock *prim = BasicBlock::Create(ctx, "prim", f), *afterPrim = BasicBlock::Create(ctx, "after_prim", f);
		BasicBlock *done = BasicBlock::Create(ctx, "done", f);
		b.CreateCondBr(b.CreateICmpULT(lane, vertices), pos, afterPos);
		b.SetInsertPoint(pos);
		Value *r = callAir(b.CreateAdd(f->getArg(kVertexId), f->getArg(kFirstVertex)));
		exportF32(12, r->getType()->isStructTy() ? b.CreateExtractValue(r, static_cast<unsigned>(position)) : r, false);   // pos0
		std::vector<Value *> values;
		for (const Var &v : vars)
			values.push_back(toVec4(b.CreateExtractValue(r, v.index)));
		b.CreateBr(afterPos);
		b.SetInsertPoint(afterPos);
		for (Value *&v : values) {   // the padding lanes store zeros
			PHINode *phi = b.CreatePHI(v4f32, 2);
			phi->addIncoming(v, pos);
			phi->addIncoming(Constant::getNullValue(v4f32), entry);
			v = phi;
		}
		b.CreateCondBr(b.CreateICmpULT(lane, b.CreateAnd(b.CreateLShr(info, 8), 255)), prim, afterPrim);
		b.SetInsertPoint(prim);
		Value *undef = PoisonValue::get(i32);
		b.CreateIntrinsic(Intrinsic::amdgcn_exp, {i32}, {b.getInt32(20), b.getInt32(1), f->getArg(kPrim), undef, undef, undef, b.getTrue(), b.getFalse()});
		b.CreateBr(afterPrim);
		b.SetInsertPoint(afterPrim);
		if (values.empty()) {
			b.CreateBr(done);
		} else {   // lanes [0, (vertices + 7) & ~7): whole vec4s in groups of 8 lanes (nggcol.s)
			BasicBlock *store = BasicBlock::Create(ctx, "store", f, done);
			b.CreateCondBr(b.CreateICmpULT(lane, b.CreateAnd(b.CreateAdd(vertices, b.getInt32(7)), b.getInt32(~7u))), store, done);
			b.SetInsertPoint(store);
			auto literal = [&](uint32_t marker) {   // s_mov_b32 with the marker as its literal, for the host to patch
				InlineAsm *mov = InlineAsm::get(FunctionType::get(i32, false), "s_mov_b32 $0, " + std::to_string(marker), "=s", true);
				return b.CreateCall(mov);
			};
			Value *rsrc = PoisonValue::get(FixedVectorType::get(i32, 4));
			Value *dw[4] = {literal(0xc01a0001u), literal(0xc01a0002u), literal(0xc01a0003u), b.getInt32(0x0043ffac)};   // dword3 as nggcol.s
			for (unsigned k = 0; k < 4; k++)
				rsrc = b.CreateInsertElement(rsrc, dw[k], k);
			Value *thread = b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {b.getInt32(-1), b.CreateShl(b.CreateAnd(b.CreateLShr(info, 24), 15), 5)});
			Value *soffset = b.CreateShl(b.CreateAnd(f->getArg(kAttrOffset), 0x7fff), 9);
			for (unsigned n = 0; n < values.size(); n++)   // slot n at offset 16 n, scope device
				b.CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store, {v4f32}, {values[n], rsrc, thread, b.getInt32(16 * n), soffset, b.getInt32(2 << 3)});
			b.CreateBr(done);
		}
		b.SetInsertPoint(done);
	} else {
		for (unsigned n = 0; n < vars.size(); n++) {   // input n's used components: P0 + P10 * i + P20 * j (pscol.s), or P0 if flat
			Value *v = PoisonValue::get(v4f32);
			for (unsigned c = 0; c < components(air->getArg(vars[n].index)->getType()); c++) {
				Value *p = b.CreateIntrinsic(Intrinsic::amdgcn_lds_param_load, {}, {b.getInt32(c), b.getInt32(n), f->getArg(kPrimMask)});
				Value *x;
				if (vars[n].flat) {   // quad lane 0 holds P0 (DPP quad_perm 0, 0, 0, 0)
					x = b.CreateBitCast(b.CreateIntrinsic(Intrinsic::amdgcn_update_dpp, {i32}, {PoisonValue::get(i32), b.CreateBitCast(p, i32), b.getInt32(0),
					                                                                            b.getInt32(15), b.getInt32(15), b.getFalse()}), f32);
				} else {
					Value *bary = f->getArg(vars[n].bary), *i = b.CreateExtractElement(bary, uint64_t(0)), *j = b.CreateExtractElement(bary, 1);
					x = b.CreateIntrinsic(Intrinsic::amdgcn_interp_inreg_p2, {}, {p, j, b.CreateIntrinsic(Intrinsic::amdgcn_interp_inreg_p10, {}, {p, i, p})});
				}
				v = b.CreateInsertElement(v, x, c);
			}
			inValues.push_back(v);
		}
		exportF32(0, callAir(nullptr), true);   // mrt0
	}
	b.CreateRetVoid();

	SmallVector<char, 0> object;
	if (std::string why = compile(m, f, air, true, object); !why.empty())
		return why;
	// The code and the registers LLVM chose (PAL metadata: amdpal.pipelines[0].registers, register number -> value).
	auto obj = object::ObjectFile::createObjectFile(MemoryBufferRef(StringRef(object.data(), object.size()), "shader"));
	auto *elf = obj ? dyn_cast<object::ELF64LEObjectFile>(obj->get()) : nullptr;
	if (!elf) {
		if (!obj)
			consumeError(obj.takeError());
		return "the shader object cannot be read";
	}
	StringRef code;
	for (const object::SectionRef &sec : elf->sections()) {
		auto name = sec.getName();
		if (name && *name == ".text") {
			if (sec.relocations().begin() != sec.relocations().end())
				return "the shader needs relocations";
			if (auto c = sec.getContents())
				code = *c;
		}
	}
	for (const object::SymbolRef &sym : elf->symbols())
		if (auto name = sym.getName(); name && *name == f->getName())
			if (auto v = sym.getValue(); !v || *v != 0)
				return "the shader's entry is not at the start of its code";
	uint32_t regs[4] = {};   // RSRC1, RSRC2, SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
	const uint64_t want[4] = {vertex ? 0x2c8au : 0x2c0au, vertex ? 0x2c8bu : 0x2c0bu, 0xa1b3, 0xa1b4};
	const auto &ef = elf->getELFFile();
	if (auto shdrs = ef.sections())
		for (const auto &sh : *shdrs) {
			if (sh.sh_type != ELF::SHT_NOTE)
				continue;
			Error err = Error::success();
			for (const auto &note : ef.notes(sh, err)) {
				msgpack::Document doc;
				if (note.getName() != "AMDGPU" || note.getType() != ELF::NT_AMDGPU_METADATA || !doc.readFromBlob(toStringRef(note.getDesc(4)), false))
					continue;
				msgpack::DocNode &pipes = doc.getRoot().getMap(true)["amdpal.pipelines"];
				if (!pipes.isArray() || pipes.getArray().size() < 1)
					continue;
				for (auto &kv : pipes.getArray()[0].getMap(true)[".registers"].getMap(true))
					for (unsigned k = 0; k < 4; k++)
						if (kv.first.getKind() == msgpack::Type::UInt && kv.first.getUInt() == want[k])
								regs[k] = static_cast<uint32_t>(kv.second.getUInt());
			}
			consumeError(std::move(err));
		}
	if (code.empty() || !regs[0])
		return "no code or no register values in the shader object";
	const uint32_t head[8] = {0x58464752 /* "RGFX" */, vertex ? 1u : 2u, static_cast<uint32_t>(code.size()), regs[0], regs[1], regs[2], regs[3],
	                          static_cast<uint32_t>(vars.size())};
	out.hsaco.assign(reinterpret_cast<const char *>(head), sizeof(head));
	out.hsaco += code.str();
	for (const Var &v : vars) {
		const uint32_t entry[2] = {v.hash, v.flat ? 1u : 0u};
		out.hsaco.append(reinterpret_cast<const char *>(entry), sizeof(entry));
	}
	out.buffers = 0;
	out.written = 0;
	return "";
}

// The kernel `fname` of `input` (AIR bitcode, or a metallib holding it) as a code object; the reason when it cannot be translated.
static std::string translate(StringRef input, StringRef fname, Kernel &out) {
	const StringRef bitcode = input.starts_with("MTLB") ? functionBitcode(input, fname) : input;
	if (bitcode.empty())
		return "no function " + fname.str() + " in the metallib";
	LLVMContext ctx;
	SMDiagnostic diag;
	std::unique_ptr<Module> m = parseIR(MemoryBufferRef(bitcode, "air"), diag, ctx);
	if (!m)
		return "cannot read the AIR: " + diag.getMessage().str();
	Function *air = m->getFunction(fname);
	for (const char *list : {"air.vertex", "air.fragment"})
		for (unsigned i = 0; air && m->getNamedMetadata(list) && i < m->getNamedMetadata(list)->getNumOperands(); i++)
			if (const MDNode *md = m->getNamedMetadata(list)->getOperand(i); mdconst::dyn_extract<Function>(md->getOperand(0)) == air)
				return translateShader(*m, air, md, list[4] == 'v', out);
	NamedMDNode *kernels = m->getNamedMetadata("air.kernel");
	const MDNode *args = nullptr;
	for (unsigned i = 0; kernels && i < kernels->getNumOperands(); i++) {
		const MDNode *k = kernels->getOperand(i);
		if (k->getNumOperands() >= 3 && mdconst::dyn_extract<Function>(k->getOperand(0)) == air)
			args = dyn_cast<MDNode>(k->getOperand(2));
	}
	if (!air || !args)
		return "no AIR kernel named " + fname.str();

	// What each argument of the AIR function is.
	enum Role { Buffer, PosInGrid, PosInGroup, GroupInGrid, GroupSize, GridSize, IndexInGroup, Lane, SimdInGroup, SimdWidth, SimdsInGroup };
	struct Arg { Role role; int location; };
	std::map<unsigned, Arg> roles;
	int maxLocation = -1;
	uint32_t written = 0;
	for (const MDOperand &op : args->operands()) {
		const auto *a = cast<MDNode>(op);
		const unsigned index = static_cast<unsigned>(mdInt(a, 0));
		const std::string kind = mdString(a, 1);
		if (kind == "air.buffer") {
			int location = -1;
			bool writes = false;
			for (unsigned k = 2; k < a->getNumOperands(); k++) {
				if (mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
				writes |= mdString(a, k) == "air.read_write" || mdString(a, k) == "air.write";
			}
			if (location < 0 || location > 31 || index >= air->arg_size() ||
			    (air->getArg(index)->getType() != PointerType::get(ctx, 1) && air->getArg(index)->getType() != PointerType::get(ctx, 2)))
				return "argument " + std::to_string(index) + ": only device and constant buffers are supported so far";
			roles[index] = {Buffer, location};
			written |= writes ? 1u << location : 0;
			maxLocation = location > maxLocation ? location : maxLocation;
		} else if (kind == "air.thread_position_in_grid") {
			roles[index] = {PosInGrid, -1};
		} else if (kind == "air.thread_position_in_threadgroup") {
			roles[index] = {PosInGroup, -1};
		} else if (kind == "air.threadgroup_position_in_grid") {
			roles[index] = {GroupInGrid, -1};
		} else if (kind == "air.threads_per_threadgroup") {
			roles[index] = {GroupSize, -1};
		} else if (kind == "air.threads_per_grid") {
			roles[index] = {GridSize, -1};
		} else if (kind == "air.thread_index_in_threadgroup") {
			roles[index] = {IndexInGroup, -1};
		} else if (kind == "air.thread_index_in_simdgroup") {
			roles[index] = {Lane, -1};
		} else if (kind == "air.simdgroup_index_in_threadgroup") {
			roles[index] = {SimdInGroup, -1};
		} else if (kind == "air.threads_per_simdgroup") {
			roles[index] = {SimdWidth, -1};
		} else if (kind == "air.simdgroups_per_threadgroup") {
			roles[index] = {SimdsInGroup, -1};
		} else {
			return "argument " + std::to_string(index) + ": " + kind + " is not supported yet";
		}
	}
	if (roles.size() != air->arg_size())
		return "not every argument of the AIR function is described";

	// The kernel: kernargs as documented above.
	const unsigned nBuffers = static_cast<unsigned>(maxLocation + 1);
	SmallVector<Type *, 40> params(nBuffers, PointerType::get(ctx, 1));
	for (unsigned k = 0; k < 6; k++)
		params.push_back(Type::getInt32Ty(ctx));
	const std::string name = air->getName().str();
	air->setName(name + ".air");
	air->setLinkage(GlobalValue::InternalLinkage);
	air->addFnAttr(Attribute::AlwaysInline);
	Function *kernel = Function::Create(FunctionType::get(Type::getVoidTy(ctx), params, false), GlobalValue::ExternalLinkage, name, m.get());
	kernel->setCallingConv(CallingConv::AMDGPU_KERNEL);
	kernel->addFnAttr("amdgpu-flat-work-group-size", "1,1024");
	IRBuilder<> b(BasicBlock::Create(ctx, "entry", kernel));
	Value *group[3], *item[3], *size[3], *actual[3], *grid[3], *pos[3];
	const Intrinsic::ID groupId[3] = {Intrinsic::amdgcn_workgroup_id_x, Intrinsic::amdgcn_workgroup_id_y, Intrinsic::amdgcn_workgroup_id_z};
	const Intrinsic::ID itemId[3] = {Intrinsic::amdgcn_workitem_id_x, Intrinsic::amdgcn_workitem_id_y, Intrinsic::amdgcn_workitem_id_z};
	Value *inside = b.getTrue();
	for (unsigned d = 0; d < 3; d++) {
		group[d] = b.CreateIntrinsic(groupId[d], {});
		item[d] = b.CreateIntrinsic(itemId[d], {});
		size[d] = kernel->getArg(nBuffers + d);
		grid[d] = kernel->getArg(nBuffers + 3 + d);
		Value *first = b.CreateMul(group[d], size[d]);
		pos[d] = b.CreateAdd(first, item[d]);
		actual[d] = b.CreateBinaryIntrinsic(Intrinsic::umin, size[d], b.CreateSub(grid[d], first));   // Metal: a partial group's own size
		inside = b.CreateAnd(inside, b.CreateICmpULT(pos[d], grid[d]));
	}
	// The thread's index in its group, x fastest: the order the hardware packs a group into waves of 32, Metal's SIMD groups.
	Value *linear = b.CreateAdd(item[0], b.CreateMul(size[0], b.CreateAdd(item[1], b.CreateMul(size[1], item[2]))));
	Value *groupThreads = b.CreateMul(actual[0], b.CreateMul(actual[1], actual[2]));
	BasicBlock *run = BasicBlock::Create(ctx, "run", kernel), *done = BasicBlock::Create(ctx, "done", kernel);
	b.CreateCondBr(inside, run, done);
	b.SetInsertPoint(run);
	SmallVector<Value *, 16> callArgs;
	for (unsigned i = 0; i < air->arg_size(); i++) {
		const Arg &a = roles[i];
		// A constant buffer (AIR's address space 2) is cast here and made global memory again by legalize().
		Value *v = a.role == Buffer      ? b.CreateAddrSpaceCast(kernel->getArg(static_cast<unsigned>(a.location)), air->getArg(i)->getType())
		         : a.role == PosInGrid   ? asType(b, air->getArg(i)->getType(), pos)
		         : a.role == PosInGroup  ? asType(b, air->getArg(i)->getType(), item)
		         : a.role == GroupInGrid ? asType(b, air->getArg(i)->getType(), group)
		         : a.role == GroupSize   ? asType(b, air->getArg(i)->getType(), actual)
		         : a.role == GridSize    ? asType(b, air->getArg(i)->getType(), grid)
		         : a.role == IndexInGroup ? linear
		         : a.role == Lane         ? b.CreateAnd(linear, 31)
		         : a.role == SimdInGroup  ? b.CreateLShr(linear, 5)
		         : a.role == SimdWidth    ? b.getInt32(32)
		                                  : b.CreateLShr(b.CreateAdd(groupThreads, b.getInt32(31)), 5);
		if (v && v->getType() != air->getArg(i)->getType())
			v = nullptr;   // the index arguments are uint
		if (!v)
			return "argument " + std::to_string(i) + ": an unsupported type for a position";
		callArgs.push_back(v);
	}
	b.CreateCall(air, callArgs);
	b.CreateBr(done);
	b.SetInsertPoint(done);
	b.CreateRetVoid();

	SmallVector<char, 0> object;
	if (std::string why = compile(*m, kernel, air, false, object); !why.empty())
		return why;
	// Link it into a code object (ld.lld -shared), through temporary files: lld's driver reads and writes files.
	SmallString<128> objPath, elfPath;
	int fd = -1;
	if (sys::fs::createTemporaryFile("air2amdgpu", "o", fd, objPath))
		return "no temporary file";
	{
		raw_fd_ostream o(fd, true);
		o << StringRef(object.data(), object.size());
	}
	if (sys::fs::createTemporaryFile("air2amdgpu", "hsaco", elfPath)) {
		sys::fs::remove(objPath);
		return "no temporary file";
	}
	std::string linkErrors;
	raw_string_ostream linkErr(linkErrors);
	const char *lldArgs[] = {"ld.lld", "-shared", objPath.c_str(), "-o", elfPath.c_str()};
	const lld::Result linked = lld::lldMain(lldArgs, outs(), linkErr, {{lld::Gnu, &lld::elf::link}});
	auto elf = MemoryBuffer::getFile(elfPath);
	sys::fs::remove(objPath);
	sys::fs::remove(elfPath);
	if (linked.retCode || !elf)
		return "ld.lld failed: " + linkErrors;
	gLldSpent |= !linked.canRunAgain;
	out.hsaco = (*elf)->getBuffer().str();
	out.buffers = nBuffers;
	out.written = written;
	return "";
}

// --serve: connect to the kext (retrying until the host is up), then translate each function it asks for. Ends when the kext's call fails
// (it went away, or this process was signalled); run it under launchd with KeepAlive.
static int serve() {
	io_connect_t conn = 0;
	for (bool said = false;; said = true) {
		const io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("RDNA4ComputeService"));
		const kern_return_t kr = svc ? IOServiceOpen(svc, mach_task_self(), RDNA4_MTLC_TYPE, &conn) : KERN_FAILURE;
		if (svc)
			IOObjectRelease(svc);
		if (kr == KERN_SUCCESS)
			break;
		if (!said)
			errs() << "air2amdgpu: waiting for the kext's translator door (0x" << Twine::utohexstr(kr) << ")\n";
		sleep(5);
	}
	errs() << "air2amdgpu: connected\n";
	std::vector<char> lib(RDNA4_MTLC_MAX_LIB);
	for (;;) {
		const uint64_t in[2] = {reinterpret_cast<uint64_t>(lib.data()), lib.size()};
		uint64_t req[2] = {};
		uint32_t nReq = 2;
		char name[64] = {};
		size_t nameLen = sizeof(name);
		const kern_return_t kr = IOConnectCallMethod(conn, kMtlcNext, in, 2, nullptr, 0, req, &nReq, name, &nameLen);
		if (kr != KERN_SUCCESS)
			return fail("the kext ended the session (0x" + Twine::utohexstr(kr).str() + ")");
		name[sizeof(name) - 1] = 0;
		Kernel k {};
		const std::string why = translate(StringRef(lib.data(), req[1]), name, k);
		errs() << "air2amdgpu: request " << req[0] << ": " << name << ": " << (why.empty() ? "translated, " + std::to_string(k.hsaco.size()) + " bytes" : why) << "\n";
		const uint64_t reply[5] = {req[0], reinterpret_cast<uint64_t>(k.hsaco.data()), why.empty() ? k.hsaco.size() : 0, k.buffers, k.written};
		IOConnectCallScalarMethod(conn, kMtlcDeliver, reply, 5, nullptr, nullptr);
		if (gLldSpent)
			return fail("lld cannot run again in this process; exiting");
	}
}

int main(int argc, char **argv) {
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmPrinters();
	InitializeAllAsmParsers();   // the inline asm of translateShader()
	if (argc == 2 && StringRef(argv[1]) == "--serve")
		return serve();
	if (argc != 4)
		return fail("usage: air2amdgpu <kernel.air|.metallib> <kernel name> <out.hsaco>  |  air2amdgpu --serve");
	auto input = MemoryBuffer::getFile(argv[1]);
	if (!input)
		return fail("cannot read " + std::string(argv[1]));
	Kernel k {};
	const std::string why = translate((*input)->getBuffer(), argv[2], k);
	if (!why.empty())
		return fail(why);
	std::error_code ec;
	raw_fd_ostream out(argv[3], ec, sys::fs::OF_None);
	if (ec)
		return fail("cannot write " + std::string(argv[3]) + ": " + ec.message());
	out << k.hsaco;
	outs() << argv[2] << " " << k.buffers << " " << k.written << "\n";
	return 0;
}
