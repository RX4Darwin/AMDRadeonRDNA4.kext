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
//   +8 * N + 24  u64 the address of the kernel's texture and sampler table (T# n at RDNA4_MTLC_TABLE_TEXTURES + 32 n, S# n at
//                RDNA4_MTLC_TABLE_SAMPLERS + 16 n, include/rdna4mtlc.h); always there, 0 without textures
// It computes the AIR function's position arguments from the work-group and work-item ids and calls the AIR function, inlined.
// Supported so far: device and constant buffers, the arguments thread_position_in_grid, thread_position_in_threadgroup,
// threadgroup_position_in_grid, threads_per_threadgroup, threads_per_grid and threadgroups_per_grid (uint, uint2 or uint3; ushort too), thread_index_in_threadgroup and the
// SIMD-group indices (a SIMD group is a wave of 32), SIMD-group functions (simdOp() below), threadgroup memory (declared, or buffer
// arguments: LDS of the kernel's own) and barriers, local
// arrays (as long as LLVM keeps them out of scratch memory, which the kext's runtime does not give kernels), device and threadgroup atomic
// read-modify-writes, textures and samplers (2D: read, write, sample at level 0), and the math and integer functions of legalize() below.
// Anything else is reported and refused.
#include "lld/Common/Driver.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/InferAddressSpaces.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <IOKit/IOKitLib.h>
#include "rtlib.h"   // kRtlib: rtlib.c as gfx1201 bitcode (build.sh)
#include <array>
#include <grp.h>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

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
// bitcode bytes). Empty if it is not there or does not fit the file. An empty name asks for the library's only function (the host sends
// one for names longer than its request holds: CoreImage's stitched kernels have names of hundreds of characters); `found` its name.
static StringRef functionBitcode(StringRef lib, StringRef name, std::string &found) {
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
		const bool match = name.empty() ? n == 1 : fname == name;

		if (match && offset != ~0ull && section + offset >= section && section + offset + bytes <= lib.size()) {
			found = fname;

			return lib.substr(section + offset, bytes);
		}
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

// One of the position arguments as the AIR function wants it (uint or ushort, scalar or up to three), from three i32 components.
static Value *asType(IRBuilder<> &b, Type *t, Value *const xyz[3]) {
	auto *v = dyn_cast<FixedVectorType>(t);
	Type *e = v ? v->getElementType() : t;
	if (!(e->isIntegerTy(32) || e->isIntegerTy(16)) || (v && v->getNumElements() > 3))
		return nullptr;
	if (!v)
		return b.CreateZExtOrTrunc(xyz[0], e);
	Value *r = PoisonValue::get(t);
	for (unsigned k = 0; k < v->getNumElements(); k++)
		r = b.CreateInsertElement(r, b.CreateZExtOrTrunc(xyz[k], e), k);
	return r;
}

// Metal's unpack_<unorm|snorm><4x8|2x16>_to_float (a u32 into float4 or float2, or half) and pack_float_to_<...> (back, rounded to
// nearest); null for the others.
static Value *packOp(IRBuilder<> &b, StringRef op, CallInst *c) {
	const bool unpack = op.consume_front("unpack."), snorm = op.starts_with("snorm");

	if (!unpack && !op.consume_front("pack."))
		return nullptr;

	const unsigned lanes = op.ends_with("4x8") ? 4 : op.ends_with("2x16") ? 2 : 0;
	const unsigned bits = 32 / (lanes ? lanes : 1);

	if (!lanes || !(snorm || op.starts_with("unorm")))
		return nullptr;

	const double scale = snorm ? (1u << (bits - 1)) - 1 : (1u << bits) - 1;
	auto *ints = FixedVectorType::get(b.getIntNTy(bits), lanes), *floats = FixedVectorType::get(b.getFloatTy(), lanes);

	if (unpack) {
		Value *v = b.CreateBitCast(c->getArgOperand(0), ints);
		v = snorm ? b.CreateSIToFP(v, floats) : b.CreateUIToFP(v, floats);
		v = b.CreateFMul(v, ConstantFP::get(floats, 1.0 / scale));

		if (snorm)
			v = b.CreateIntrinsic(Intrinsic::maxnum, {floats}, {v, ConstantFP::get(floats, -1.0)});

		return b.CreateFPCast(v, c->getType());
	}

	Value *v = b.CreateFPCast(c->getArgOperand(0), floats);
	v = b.CreateIntrinsic(Intrinsic::maxnum, {floats}, {v, ConstantFP::get(floats, snorm ? -1.0 : 0.0)});
	v = b.CreateIntrinsic(Intrinsic::minnum, {floats}, {v, ConstantFP::get(floats, 1.0)});
	v = b.CreateIntrinsic(Intrinsic::rint, {floats}, {b.CreateFMul(v, ConstantFP::get(floats, scale))});
	v = snorm ? b.CreateFPToSI(v, ints) : b.CreateFPToUI(v, ints);

	return b.CreateBitCast(v, c->getType());
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
	if (what == "is_first")   // the lowest active lane (Quake II's per-group atomics)
		return b.CreateICmpEQ(b.CreateBinaryIntrinsic(Intrinsic::cttz, ballot(b.getTrue()), b.getTrue()), lane);
	if (what == "broadcast_first" && t->getPrimitiveSizeInBits() == 32)   // the lowest active lane's value
		return b.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {t}, {x});
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
// [14:13] (0 none, 1 nearest, 2 linear), pixel coordinates [15], compare function [19:16] (MSL's order: 1 less .. 7 always, 8 never and
// the default), anisotropy [23:19] = 2n - 1, min / max LOD halves at [39:24] / [55:40], border colour [57:56].
static uint32_t lod48(uint16_t half) {   // a half's value as gfx12's 4.8 fixed LOD, clamped to [0, 15.996]
	const int e = (half >> 10) & 31, m = half & 1023;
	if ((half & 0x8000) || !e)
		return 0;
	const double v = (1.0 + m / 1024.0) * double(1u << e) / 32768.0;
	return v >= 15.996 ? 0xfff : static_cast<uint32_t>(v * 256 + 0.5);
}
static Constant *airSampler(LLVMContext &ctx, uint64_t st) {
	static const uint32_t clamp[4] = {6, 2, 0, 1};   // zero -> CLAMP_BORDER, edge -> CLAMP_LAST_TEXEL, repeat -> WRAP, mirror -> MIRROR
	// less, less_equal, greater, greater_equal, equal, not_equal, always, never -> SQ_TEX_DEPTH_COMPARE
	static const uint32_t compare[16] = {0, 1, 3, 4, 6, 2, 5, 7, 0};
	const uint32_t aniso = ((st >> 19) & 31) / 2 + 1, ratio = aniso >= 16 ? 4 : aniso >= 8 ? 3 : aniso >= 4 ? 2 : aniso >= 2 ? 1 : 0;
	const uint32_t w[4] = {
		clamp[st & 3] | clamp[(st >> 3) & 3] << 3 | clamp[(st >> 6) & 3] << 6 | ratio << 9 | compare[(st >> 16) & 15] << 12 |
			uint32_t((st >> 15) & 1) << 15,
		lod48(uint16_t(st >> 24)) | lod48(uint16_t(st >> 40)) << 13,
		uint32_t((st >> 9) & 1) << 20 | uint32_t((st >> 11) & 1) << 22 | uint32_t((st >> 13) & 3) << 26,
		uint32_t((st >> 56) & 3) << 30};   // BORDER_COLOR_TYPE: 0 transparent black, 1 opaque black, 2 opaque white
	Type *i32 = Type::getInt32Ty(ctx);
	return ConstantVector::get({ConstantInt::get(i32, w[0]), ConstantInt::get(i32, w[1]), ConstantInt::get(i32, w[2]), ConstantInt::get(i32, w[3])});
}

// `t` with AIR's constant address space (2) replaced by the global one in pointers inside structs, arrays and vectors as well: CoreImage
// carries a texture and its sampler in a struct (coreimage::Sampler), whose pointer fields must change with the values put into them, or
// insertvalue and extractvalue no longer match (ci_lanczosDown2: "the retargeted module does not verify", blank app icons).
static Type *globalType(Type *t, std::map<Type *, Type *> &done) {
	if (auto it = done.find(t); it != done.end())
		return it->second;

	Type *r = t;

	if (t->isPointerTy() && t->getPointerAddressSpace() == 2) {
		r = PointerType::get(t->getContext(), 1);
	} else if (auto *st = dyn_cast<StructType>(t); st && !st->isOpaque()) {
		SmallVector<Type *, 8> el;
		bool changed = false;

		for (Type *e : st->elements()) {
			el.push_back(globalType(e, done));
			changed |= el.back() != e;
		}

		if (changed)
			r = st->isLiteral() ? StructType::get(t->getContext(), el, st->isPacked())
			                    : StructType::create(t->getContext(), el, (st->getName() + ".global").str(), st->isPacked());
	} else if (auto *at = dyn_cast<ArrayType>(t)) {
		Type *e = globalType(at->getElementType(), done);

		r = e != at->getElementType() ? ArrayType::get(e, at->getNumElements()) : t;
	} else if (auto *vt = dyn_cast<FixedVectorType>(t)) {
		Type *e = globalType(vt->getElementType(), done);

		r = e != vt->getElementType() ? FixedVectorType::get(e, vt->getNumElements()) : t;
	}

	done[t] = r;

	return r;
}

// Constant tables (an initialized global in AIR's constant address space: kitty's quad corners through AppleMetalOpenGLRenderer,
// 2026-11-12): each load from one becomes the value, chosen by the index with selects when one index is not constant. A graphics
// shader's code is copied out of its object without data or relocations, so a table cannot stay in memory. Up to 64 entries; any other
// use stays and is refused below.
static void lowerTables(Function &k) {
	const DataLayout &dl = k.getParent()->getDataLayout();
	SmallVector<GlobalVariable *, 4> tables;

	for (GlobalVariable &g : k.getParent()->globals())
		if (g.getAddressSpace() == 2 && g.isConstant() && g.hasInitializer())
			tables.push_back(&g);
	for (GlobalVariable *g : tables) {
		convertUsersOfConstantsToInstructions({g}, &k);

		SmallVector<Instruction *, 8> gone;
		const SmallVector<User *, 8> users(g->users());

		for (User *u : users) {
			auto *gep = dyn_cast<GetElementPtrInst>(u);
			auto *direct = dyn_cast<LoadInst>(u);

			if ((gep ? gep->getFunction() : direct ? direct->getFunction() : nullptr) != &k)
				continue;

			// the dynamic index, if any: one at most
			int dynamicAt = -1;
			bool ok = true;

			if (gep)
				for (unsigned i = 1; i < gep->getNumOperands() && ok; i++)
					if (!isa<ConstantInt>(gep->getOperand(i))) {
						ok = dynamicAt < 0;
						dynamicAt = static_cast<int>(i);
					}

			const SmallVector<User *, 8> loads(gep ? SmallVector<User *, 8>(gep->users()) : SmallVector<User *, 8>{direct});

			for (User *lu : loads)
				ok = ok && isa<LoadInst>(lu) && cast<LoadInst>(lu)->getPointerOperand() == (gep ? static_cast<Value *>(gep) : g);

			// the elements the dynamic index chooses among: the array it indexes
			uint64_t n = 1;

			if (ok && dynamicAt > 0) {
				SmallVector<Value *, 4> head(gep->idx_begin(), gep->idx_begin() + (dynamicAt - 1));
				auto *at = dyn_cast<ArrayType>(GetElementPtrInst::getIndexedType(gep->getSourceElementType(), head));

				n = at ? at->getNumElements() : 0;
				ok = n && n <= 64;
			}
			if (!ok)
				continue;

			for (User *lu : loads) {
				auto *l = cast<LoadInst>(lu);
				auto element = [&](uint64_t i) -> Constant * {
					if (!gep)
						return ConstantFoldLoadFromConstPtr(g, l->getType(), dl);

					SmallVector<Constant *, 4> idx;

					for (unsigned o = 1; o < gep->getNumOperands(); o++)
						idx.push_back(static_cast<int>(o) == dynamicAt ? ConstantInt::get(gep->getOperand(o)->getType(), i)
						                                               : cast<Constant>(gep->getOperand(o)));
					return ConstantFoldLoadFromConstPtr(ConstantExpr::getGetElementPtr(gep->getSourceElementType(), g, idx), l->getType(), dl);
				};
				Value *v = element(dynamicAt > 0 ? n - 1 : 0);

				if (dynamicAt > 0 && v) {
					IRBuilder<> b(l);
					Value *index = gep->getOperand(static_cast<unsigned>(dynamicAt));

					for (uint64_t i = n - 1; i-- > 0 && v;) {
						Constant *e = element(i);

						v = e ? b.CreateSelect(b.CreateICmpEQ(index, ConstantInt::get(index->getType(), i)), e, v) : nullptr;
					}
				}
				if (!v)
					continue;
				l->replaceAllUsesWith(v);
				gone.push_back(l);
			}
		}
		for (Instruction *i : gone)
			i->eraseFromParent();

		SmallVector<Instruction *, 8> deadGeps;   // the loads were all they had
		for (User *u : SmallVector<User *, 8>(g->users()))
			if (auto *gep = dyn_cast<GetElementPtrInst>(u); gep && gep->use_empty() && gep->getFunction() == &k)
				deadGeps.push_back(gep);
		for (Instruction *i : deadGeps)
			i->eraseFromParent();
	}
}

// Metal's ray query (air.intersect.triangle_data, air.intersect.instancing[.triangle_data[.world_space_data]]: origin, direction, minimum
// and maximum distance, the acceleration structure, with instancing the mask, ..., accept any intersection as the last i1) as a call of
// rtlib.c's rdna4_rt_intersect, which linkRayQueries links in after retargeting; its eight floats into AIR's result {i32 type, float
// distance, i32 primitive id, i32 geometry index, ptr primitive data (null)}, then with instancing {i32 instance id, i32 user instance id
// (the same)}, with triangle data {<2 x float> barycentrics, i1 front facing}, with world space data the hit instance's world-to-object and
// object-to-world transforms as 4 + 4 float3 columns (rdna4_rt_world_to_object, rdna4_rt_object_to_world). Null for another shape.
static Value *rayQuery(IRBuilder<> &b, CallInst *c, StringRef name) {
	Module &m = *c->getModule();
	LLVMContext &ctx = m.getContext();
	Type *f32 = b.getFloatTy(), *i32 = b.getInt32Ty();
	auto *st = dyn_cast<StructType>(c->getType());
	auto *v2 = FixedVectorType::get(f32, 2), *v3 = FixedVectorType::get(f32, 3);
	const bool instances = name.starts_with("intersect.instancing"), triangles = name.contains("triangle_data"),
	           world = name.ends_with(".world_space_data");
	const unsigned at = instances ? 7 : 5;   // the triangle data's first field
	const unsigned fields = at + (triangles ? 2 : 0) + (world ? 8 : 0);
	if (!st || st->getNumElements() != fields || st->getElementType(0) != i32 || st->getElementType(1) != f32 || !st->getElementType(4)->isPointerTy() ||
	    (instances && (st->getElementType(5) != i32 || st->getElementType(6) != i32)) ||
	    (triangles && (st->getElementType(at) != v2 || !st->getElementType(at + 1)->isIntegerTy(1))) || c->arg_size() < 7 ||
	    c->getArgOperand(0)->getType() != v3 || c->getArgOperand(1)->getType() != v3 || !c->getArgOperand(4)->getType()->isPointerTy() ||
	    (instances && c->getArgOperand(5)->getType() != i32) || !c->getArgOperand(c->arg_size() - 1)->getType()->isIntegerTy(1))
		return nullptr;
	for (unsigned i = at + (triangles ? 2 : 0); i < fields; i++)
		if (st->getElementType(i) != v3)
			return nullptr;
	Type *global = PointerType::get(ctx, 1);
	FunctionCallee fn = m.getOrInsertFunction("rdna4_rt_intersect", FunctionType::get(FixedVectorType::get(f32, 8),
	                                                                                 {f32, f32, f32, f32, f32, f32, f32, f32, global, i32, i32}, false));
	Value *as = b.CreatePointerCast(c->getArgOperand(4), global);
	SmallVector<Value *, 11> args;
	for (unsigned v = 0; v < 2; v++)
		for (unsigned i = 0; i < 3; i++)
			args.push_back(b.CreateExtractElement(c->getArgOperand(v), i));
	args.append({c->getArgOperand(2), c->getArgOperand(3), as, b.CreateZExt(c->getArgOperand(c->arg_size() - 1), i32),
	             instances ? c->getArgOperand(5) : b.getInt32(~0u)});
	Value *h = b.CreateCall(fn, args);
	auto lane = [&](unsigned i) { return b.CreateExtractElement(h, i); };
	auto bitsOf = [&](unsigned i) { return b.CreateBitCast(lane(i), i32); };
	Value *r = PoisonValue::get(st);
	r = b.CreateInsertValue(r, bitsOf(0), 0);
	r = b.CreateInsertValue(r, lane(1), 1);
	r = b.CreateInsertValue(r, bitsOf(2), 2);
	r = b.CreateInsertValue(r, bitsOf(3), 3);
	r = b.CreateInsertValue(r, ConstantPointerNull::get(cast<PointerType>(st->getElementType(4))), 4);
	if (instances)   // the instance hit (lane 7), as its id and its user id
		r = b.CreateInsertValue(b.CreateInsertValue(r, bitsOf(7), 5), bitsOf(7), 6);
	if (triangles) {
		Value *bary = b.CreateInsertElement(b.CreateInsertElement(PoisonValue::get(v2), lane(4), uint64_t(0)), lane(5), 1);
		r = b.CreateInsertValue(b.CreateInsertValue(r, bary, at), b.CreateTrunc(bitsOf(6), b.getInt1Ty()), at + 1);
	}
	if (world) {
		auto *v16 = FixedVectorType::get(f32, 16);
		const char *names[2] = {"rdna4_rt_world_to_object", "rdna4_rt_object_to_world"};
		for (unsigned t = 0; t < 2; t++) {
			Value *mat = b.CreateCall(m.getOrInsertFunction(names[t], FunctionType::get(v16, {global, i32}, false)), {as, bitsOf(7)});
			for (unsigned col = 0; col < 4; col++)
				r = b.CreateInsertValue(r, b.CreateShuffleVector(mat, {int(3 * col), int(3 * col + 1), int(3 * col + 2)}), at + 2 + 4 * t + col);
		}
	}
	return r;
}

// rtlib.c into a module that calls it (rayQuery), as gfx1201 bitcode of the same LLVM; the optimiser inlines it. "" or the reason.
static std::string linkRayQueries(Module &m) {
	Function *rq = m.getFunction("rdna4_rt_intersect");
	if (!rq || !rq->isDeclaration())
		return "";
	SMDiagnostic diag;
	std::unique_ptr<Module> lib = parseIR(MemoryBufferRef(StringRef(reinterpret_cast<const char *>(kRtlib), sizeof(kRtlib)), "rtlib"), diag, m.getContext());
	if (!lib)
		return "the ray query library does not load: " + diag.getMessage().str();
	lib->setTargetTriple(m.getTargetTriple());
	lib->setDataLayout(m.getDataLayout());
	if (Linker::linkModules(m, std::move(lib), Linker::LinkOnlyNeeded))
		return "the ray query library does not link";
	// inlined here and gone: a shader is one function, its entry at the start of its code (fragment shadow rays, MetalQuake)
	for (const char *name : {"rdna4_rt_intersect", "rdna4_rt_world_to_object", "rdna4_rt_object_to_world"}) {
		Function *linked = m.getFunction(name);
		if (!linked)
			continue;
		for (User *u : make_early_inc_range(linked->users()))
			if (auto *call = dyn_cast<CallBase>(u)) {
				InlineFunctionInfo info;
				if (InlineResult done = InlineFunction(*call, info); !done.isSuccess())
					return std::string("a ray query does not inline: ") + done.getFailureReason();
			}
		if (linked->use_empty())
			linked->eraseFromParent();
	}
	return "";
}

static std::string legalize(Function &k) {
	LLVMContext &ctx = k.getContext();
	PointerType *global = PointerType::get(ctx, 1);
	SmallVector<Instruction *, 32> gone;
	// Constexpr samplers first: their state global (AIR's constant address space) becomes the S# constant of the call. Every sampling call's,
	// depth ones too (MetalGraphicsPlayground's shadow lookups: air.sample_depth_2d refused as "a constant table", 2026-11-24).
	std::map<CallInst *, Constant *> constSampler;
	for (Instruction &i : instructions(k))
		if (auto *c = dyn_cast<CallInst>(&i); c && c->getCalledFunction() && c->arg_size() > 1 &&
		                                        (c->getCalledFunction()->getName().starts_with("air.sample_") ||
		                                         c->getCalledFunction()->getName().starts_with("air.gather_")))
			if (auto *g = dyn_cast<GlobalVariable>(c->getArgOperand(1)->stripPointerCasts()); g && g->hasInitializer()) {
				// The state word: an i64, or the first of an array of them (both seen from the Metal compiler).
				auto *a = dyn_cast<ConstantDataArray>(g->getInitializer());
				auto *one = dyn_cast<ConstantInt>(g->getInitializer());
				if ((a && a->getNumElements() >= 1 && a->getElementType()->isIntegerTy(64)) || (one && one->getBitWidth() == 64)) {
					constSampler[c] = airSampler(ctx, a ? a->getElementAsInteger(0) : one->getZExtValue());
					c->setArgOperand(1, ConstantPointerNull::get(cast<PointerType>(c->getArgOperand(1)->getType())));
				}
			} else if (auto *e = dyn_cast<ConstantExpr>(c->getArgOperand(1)); e && e->getOpcode() == Instruction::IntToPtr) {
				// older AIR's literal sampler (Cubase's blits): the state word itself as the pointer, bit 63 set
				if (auto *word = dyn_cast<ConstantInt>(e->getOperand(0)); word && word->getBitWidth() == 64) {
					constSampler[c] = airSampler(ctx, word->getZExtValue());
					c->setArgOperand(1, ConstantPointerNull::get(cast<PointerType>(c->getArgOperand(1)->getType())));
				}
			}
	lowerTables(k);   // after the samplers: theirs are constant globals too
	std::map<Type *, Type *> globalTypes;
	for (Instruction &i : instructions(k)) {
		for (Use &u : i.operands()) {
			Type *from = u->getType(), *to = globalType(from, globalTypes);

			if (from == to || !isa<Constant>(u.get()))
				continue;

			if (isa<ConstantPointerNull>(u.get()) || isa<ConstantAggregateZero>(u.get()))
				u.set(Constant::getNullValue(to));
			else if (isa<PoisonValue>(u.get()))
				u.set(PoisonValue::get(to));
			else if (isa<UndefValue>(u.get()))
				u.set(from->isPointerTy() ? static_cast<Value *>(PoisonValue::get(to)) : UndefValue::get(to));
			else {
				std::string what;
				raw_string_ostream os(what);
				os << "data in AIR's constant address space (a constant table) is not supported yet: ";
				u.get()->printAsOperand(os, false);
				os << " in";
				i.print(os);
				return os.str();
			}
		}

		if (Type *to = globalType(i.getType(), globalTypes); to != i.getType())
			i.mutateType(to);
		// The types a GEP steps through and an alloca holds as well: AMDGPU's address space 2 has 32-bit pointers, so a struct of AIR's
		// constant pointers kept its layout's offsets halved (MoltenVK's argument buffers: a vertex shader read its UBO's address from
		// offset 4 of spvDescriptorSetBuffer2 instead of 8 and hung the gfx ring, vkQuake 2026-11-21).
		if (auto *gep = dyn_cast<GetElementPtrInst>(&i)) {
			gep->setSourceElementType(globalType(gep->getSourceElementType(), globalTypes));
			gep->setResultElementType(globalType(gep->getResultElementType(), globalTypes));
		} else if (auto *a = dyn_cast<AllocaInst>(&i)) {
			a->setAllocatedType(globalType(a->getAllocatedType(), globalTypes));
		}
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
		{"min.u", Intrinsic::umin}, {"max.s", Intrinsic::smax}, {"max.u", Intrinsic::umax}, {"reverse_bits", Intrinsic::bitreverse}};
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
		if (name == "get_null_intersection_function_table") {   // no intersection functions: rtlib.c's traversal takes none
			c->replaceAllUsesWith(ConstantPointerNull::get(cast<PointerType>(c->getType())));
			gone.push_back(c);
			continue;
		}
		if (name == "intersect.triangle_data" || name.starts_with("intersect.instancing")) {
			Value *hit = rayQuery(b, c, name);
			if (!hit)
				return f->getName().str() + " of a form not supported yet";
			c->replaceAllUsesWith(hit);
			gone.push_back(c);
			continue;
		}
		// A descriptor (T# 8 dwords, S# 4) at the handle's address, through the constant address space so it lands in SGPRs.
		auto descriptor = [&](Value *handle, unsigned dwords) {
			Value *at = b.CreateIntToPtr(b.CreatePtrToInt(handle, b.getInt64Ty()), PointerType::get(ctx, 4));
			return b.CreateAlignedLoad(FixedVectorType::get(b.getInt32Ty(), dwords), at, Align(16));
		};
		// Texture calls with ushort coordinates (".i16" after the name) are the uint ones with their coordinates widened.
		std::string texName = name.str();
		if (const size_t i16 = texName.find("_2d.i16."); i16 != std::string::npos)
			texName.erase(i16 + 3, 4);
		const StringRef tex = texName;
		const bool readInt = tex.starts_with("read_texture_2d.u.v4i") || tex.starts_with("read_texture_2d.s.v4i");
		if (tex.starts_with("sample_texture_2d.v4f") || tex.starts_with("sample_texture_3d.v4f") || tex.starts_with("read_texture_2d.v4f") || readInt) {
			// sample (texture, sampler, float2 coord, i1, int2 offset, i1 level, float bias or LOD, float min LOD, i32) / read (texture,
			// sampler, uint2, int2, lod, i32), or without the sampler and the offset (texture, uint2, lod, i32: WindowServer's). A sample's
			// level false: the float is a bias (0: none); true: an explicit LOD. Float or half texels. A 3D texture's sample: float3, int3.
			const bool sample = tex.starts_with("sample"), cube = tex.starts_with("sample_texture_3d");
			const bool half = tex.starts_with(sample ? (cube ? "sample_texture_3d.v4f16" : "sample_texture_2d.v4f16") : "read_texture_2d.v4f16");
			const bool samplerless = !sample && c->arg_size() >= 3 && c->getArgOperand(1)->getType()->isVectorTy();
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(samplerless ? 1 : 2);
			Value *x = b.CreateExtractElement(xy, uint64_t(0)), *y = b.CreateExtractElement(xy, 1);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel;
			if (sample) {
				// (the oldest AIR, Cubase's: seven operands, ending with the bias or LOD)
				auto *offset = c->arg_size() > 6 ? dyn_cast<Constant>(c->getArgOperand(4)) : nullptr;
				auto *level = c->arg_size() > 6 ? dyn_cast<ConstantInt>(c->getArgOperand(5)) : nullptr;
				Value *value = c->arg_size() > 6 ? c->getArgOperand(6) : nullptr;
				if (!offset || !level)
					return f->getName().str() + " with a run-time offset or level kind is not supported yet";
				// A minimum LOD (min_lod_clamp): the _cl forms for an implicit LOD or a bias, an explicit LOD raised to it; outside a fragment
				// shader (level 0 otherwise) the minimum itself as the explicit LOD. Older AIR (Waves' plugin windows) has no minimum: its
				// eighth operand is the trailing i32, and it returns the texel alone.
				Value *minLod = c->arg_size() > 7 ? c->getArgOperand(7) : nullptr;
				const bool clamped = minLod && minLod->getType()->isFloatTy() && !match(minLod, m_AnyZeroFP()),
				           fragment = k.getCallingConv() == CallingConv::AMDGPU_PS;
				if (clamped && (level->isOne() || !fragment)) {
					value = level->isOne() ? b.CreateBinaryIntrinsic(Intrinsic::maxnum, value, minLod) : minLod;
					level = b.getTrue();
				}
				// The offset as the hardware takes it: x [5:0], y [13:8], 6-bit signed.
				uint32_t packed = 0;
				for (unsigned k = 0; k < (cube ? 3u : 2u); k++)
					if (auto *e = dyn_cast_or_null<ConstantInt>(offset->getAggregateElement(k)))
						packed |= (static_cast<uint32_t>(e->getSExtValue()) & 0x3f) << (8 * k);
				const bool zero = match(value, m_AnyZeroFP()), lod = level->isOne(), bias = !lod && !zero;
				static const Intrinsic::ID ids[2][2][4] = {   // [3D][offset][plain, bias, lod, lod zero]
					{{Intrinsic::amdgcn_image_sample_2d, Intrinsic::amdgcn_image_sample_b_2d, Intrinsic::amdgcn_image_sample_l_2d, Intrinsic::amdgcn_image_sample_lz_2d},
					 {Intrinsic::amdgcn_image_sample_o_2d, Intrinsic::amdgcn_image_sample_b_o_2d, Intrinsic::amdgcn_image_sample_l_o_2d, Intrinsic::amdgcn_image_sample_lz_o_2d}},
					{{Intrinsic::amdgcn_image_sample_3d, Intrinsic::amdgcn_image_sample_b_3d, Intrinsic::amdgcn_image_sample_l_3d, Intrinsic::amdgcn_image_sample_lz_3d},
					 {Intrinsic::amdgcn_image_sample_o_3d, Intrinsic::amdgcn_image_sample_b_o_3d, Intrinsic::amdgcn_image_sample_l_o_3d, Intrinsic::amdgcn_image_sample_lz_o_3d}}};
				// Only a fragment shader has pixel quads to take derivatives from: elsewhere (vertex shaders, kernels) implicit-LOD samples
				// (and biases) read level 0.
				const bool quads = k.getCallingConv() == CallingConv::AMDGPU_PS;
				const unsigned mode = !quads && !(lod && !zero) ? 3 : bias ? 1 : lod ? (zero ? 3 : 2) : 0;
				const bool cl = clamped && mode < 2;   // (an implicit LOD or a bias in a fragment shader, its minimum after the coordinates)
				static const Intrinsic::ID clIds[2][2][2] = {   // [3D][offset][plain, bias]
					{{Intrinsic::amdgcn_image_sample_cl_2d, Intrinsic::amdgcn_image_sample_b_cl_2d},
					 {Intrinsic::amdgcn_image_sample_cl_o_2d, Intrinsic::amdgcn_image_sample_b_cl_o_2d}},
					{{Intrinsic::amdgcn_image_sample_cl_3d, Intrinsic::amdgcn_image_sample_b_cl_3d},
					 {Intrinsic::amdgcn_image_sample_cl_o_3d, Intrinsic::amdgcn_image_sample_b_cl_o_3d}}};
				Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
				SmallVector<Type *, 5> types = {v4f};   // the overloads: the texel, [the bias], the coordinates, the descriptors
				SmallVector<Value *, 11> args = {b.getInt32(15)};
				if (packed)
					args.push_back(b.getInt32(packed));
				if (mode == 1) {
					types.push_back(b.getFloatTy());
					args.push_back(value);
				}
				types.push_back(b.getFloatTy());
				args.append({x, y});
				if (cube)
					args.push_back(b.CreateExtractElement(xy, 2));
				if (mode == 2)
					args.push_back(value);
				if (cl)
					args.push_back(minLod);
				types.append({t->getType(), sm->getType()});
				args.append({t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
				const CallingConv::ID cc = k.getCallingConv();
				if (mode == 2 && !cube && (cc == CallingConv::AMDGPU_PS || cc == CallingConv::AMDGPU_GS)) {
					// An explicit LOD in a draw's shader (WindowServer's glass blurs each level from the one before, Tahoe): levels
					// floor(lod) and the next as images of their own (RDNA4_MTLC_LEVEL_FIRST), sampled at their level 0, blended by the
					// LOD's fraction, as a linear mip filter. ponytail: always blended (a nearest mip filter too), the sampler's LOD
					// clamps ignored; kernels' tables have no levels (their explicit LODs read level 0 as before).
					k.addFnAttr("rdna4-levels");
					Type *ft = b.getFloatTy();
					Value *l = b.CreateBinaryIntrinsic(Intrinsic::minnum, b.CreateBinaryIntrinsic(Intrinsic::maxnum, value, ConstantFP::get(ft, 0.0)),
					                                   ConstantFP::get(ft, RDNA4_MTLC_LEVELS - 1.0));
					Value *whole = b.CreateUnaryIntrinsic(Intrinsic::floor, l), *fraction = b.CreateFSub(l, whole);
					Value *l0 = b.CreateFPToUI(whole, b.getInt32Ty());
					Value *l1 = b.CreateBinaryIntrinsic(Intrinsic::umin, b.CreateAdd(l0, b.getInt32(1)), b.getInt32(RDNA4_MTLC_LEVELS - 1));
					auto levelSample = [&](Value *level) {
						Value *past = b.CreateAdd(b.CreateMul(level, b.getInt32(RDNA4_MTLC_LEVEL_STRIDE)),
						                          b.getInt32(RDNA4_MTLC_LEVEL_FIRST - RDNA4_MTLC_LEVEL_STRIDE));
						Value *at = b.CreateSelect(b.CreateICmpEQ(level, b.getInt32(0)), b.getInt32(0), past);
						Value *lt = descriptor(b.CreateGEP(b.getInt8Ty(), c->getArgOperand(0), b.CreateZExt(at, b.getInt64Ty())), 8);
						SmallVector<Value *, 10> la = {b.getInt32(15)};
						if (packed)
							la.push_back(b.getInt32(packed));
						la.append({x, y, lt, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
						return b.CreateIntrinsic(ids[0][packed != 0][3], {v4f, ft, lt->getType(), sm->getType()}, la);
					};
					Value *s0 = levelSample(l0), *s1 = levelSample(l1);
					texel = b.CreateFAdd(s0, b.CreateFMul(b.CreateFSub(s1, s0), b.CreateVectorSplat(4, fraction)));
				} else
					texel = b.CreateIntrinsic(cl ? clIds[cube][packed != 0][mode] : ids[cube][packed != 0][mode], types, args);
			} else {
				// The offset is added to the coordinates; a level other than 0 reads that mip level (image_load_mip).
				if (!samplerless && c->arg_size() < 5)
					return f->getName().str() + " without an offset and a LOD is not known";
				Value *offset = samplerless ? ConstantAggregateZero::get(xy->getType()) : c->getArgOperand(3);
				Value *lod = c->getArgOperand(samplerless ? 2 : 4);
				x = b.CreateZExt(x, b.getInt32Ty());   // uint or ushort
				y = b.CreateZExt(y, b.getInt32Ty());

				if (!isa<ConstantAggregateZero>(offset)) {
					x = b.CreateAdd(x, b.CreateSExtOrTrunc(b.CreateExtractElement(offset, uint64_t(0)), b.getInt32Ty()));
					y = b.CreateAdd(y, b.CreateSExtOrTrunc(b.CreateExtractElement(offset, 1), b.getInt32Ty()));
				}

				if (match(lod, m_Zero()))
					texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2d, {v4f, b.getInt32Ty(), t->getType()},
					                          {b.getInt32(15), x, y, t, b.getInt32(0), b.getInt32(0)});
				else
					texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_load_mip_2d, {v4f, b.getInt32Ty(), t->getType()},
					                          {b.getInt32(15), x, y, b.CreateZExtOrTrunc(lod, b.getInt32Ty()), t, b.getInt32(0), b.getInt32(0)});
			}
			if (half)
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			// an integer texture's uint4 / int4 (or ushort4 / short4): the loaded float4's bits, as integer texels are stored
			if (readInt)
				texel = b.CreateIntCast(b.CreateBitCast(texel, FixedVectorType::get(b.getInt32Ty(), 4)),
				                        cast<StructType>(c->getType())->getElementType(0), tex.starts_with("read_texture_2d.s"));
			// {texel, i8 0}, or older AIR's texel alone
			r = c->getType()->isStructTy() ? b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1) : texel;
		} else if (tex.starts_with("sample_texture_2d_grad.v4f")) {
			// (texture, sampler, float2 coord, float2 dPdx, float2 dPdy, float min LOD, i1, int2 offset, i32): explicit gradients
			// (Unity's xlatMtlMain shaders, Daggerfall Unity) as image_sample_d: dsdh, dtdh = dPdx, dsdv, dtdv = dPdy.
			auto *offset = dyn_cast<Constant>(c->getArgOperand(7));
			if (!offset || !match(c->getArgOperand(5), m_AnyZeroFP()))
				return f->getName().str() + " with a run-time offset or a minimum LOD is not supported yet";
			uint32_t packed = 0;
			for (unsigned i = 0; i < 2; i++)
				if (auto *e = dyn_cast_or_null<ConstantInt>(offset->getAggregateElement(i)))
					packed |= (static_cast<uint32_t>(e->getSExtValue()) & 0x3f) << (8 * i);
			Value *t = descriptor(c->getArgOperand(0), 8);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Value *xy = c->getArgOperand(2), *dx = c->getArgOperand(3), *dy = c->getArgOperand(4);
			Type *ft = b.getFloatTy(), *v4f = FixedVectorType::get(ft, 4);
			SmallVector<Value *, 13> args = {b.getInt32(15)};
			if (packed)
				args.push_back(b.getInt32(packed));
			args.append({b.CreateExtractElement(dx, uint64_t(0)), b.CreateExtractElement(dx, 1), b.CreateExtractElement(dy, uint64_t(0)),
			             b.CreateExtractElement(dy, 1), b.CreateExtractElement(xy, uint64_t(0)), b.CreateExtractElement(xy, 1), t, sm,
			             b.getFalse(), b.getInt32(0), b.getInt32(0)});
			Value *texel = b.CreateIntrinsic(packed ? Intrinsic::amdgcn_image_sample_d_o_2d : Intrinsic::amdgcn_image_sample_d_2d,
			                                 {v4f, ft, ft, t->getType(), sm->getType()}, args);
			if (tex.starts_with("sample_texture_2d_grad.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);
		} else if (tex.starts_with("sample_depth_2d.f32") || tex.starts_with("sample_compare_depth_2d.f32")) {
			// depth2d: sample (texture, sampler, i32, float2 coord, i1, int2 offset, i1 level, float bias or LOD, float min LOD, i32) and
			// sample_compare with the reference after the coordinates (the sampler's DEPTH_COMPARE_FUNC: ref OP texel, 1.0 or 0.0). The
			// texture's T# is its depth as 32_FLOAT, 64KB_2D (RDNA4MTLTexture descriptor:). Shadow maps (Daggerfall Unity).
			const bool compare = tex.starts_with("sample_compare");
			const unsigned at = compare ? 5 : 4;   // the i1 after the coordinates (and the reference)
			auto *offset = dyn_cast<Constant>(c->getArgOperand(at + 1));
			auto *level = dyn_cast<ConstantInt>(c->getArgOperand(at + 2));
			Value *value = c->getArgOperand(at + 3);
			if (!offset || !level || !match(c->getArgOperand(at + 4), m_AnyZeroFP()))
				return f->getName().str() + " with a run-time offset or level kind, or a minimum LOD, is not supported yet";
			uint32_t packed = 0;
			for (unsigned i = 0; i < 2; i++)
				if (auto *e = dyn_cast_or_null<ConstantInt>(offset->getAggregateElement(i)))
					packed |= (static_cast<uint32_t>(e->getSExtValue()) & 0x3f) << (8 * i);
			const bool zero = match(value, m_AnyZeroFP()), lod = level->isOne(), bias = !lod && !zero;
			const bool quads = k.getCallingConv() == CallingConv::AMDGPU_PS;   // implicit LODs need pixel quads (as the 2D case)
			// plain, bias (GL's shadow lookups through Apple's GL-on-Metal renderer: OpenMW's shadows), explicit LOD, LOD 0
			const unsigned mode = !quads || (lod && zero) ? 3 : bias ? 1 : lod ? 2 : 0;
			static const Intrinsic::ID ids[2][2][4] = {   // [compare][offset][plain, bias, lod, lod zero]
				{{Intrinsic::amdgcn_image_sample_2d, Intrinsic::amdgcn_image_sample_b_2d, Intrinsic::amdgcn_image_sample_l_2d, Intrinsic::amdgcn_image_sample_lz_2d},
				 {Intrinsic::amdgcn_image_sample_o_2d, Intrinsic::amdgcn_image_sample_b_o_2d, Intrinsic::amdgcn_image_sample_l_o_2d, Intrinsic::amdgcn_image_sample_lz_o_2d}},
				{{Intrinsic::amdgcn_image_sample_c_2d, Intrinsic::amdgcn_image_sample_c_b_2d, Intrinsic::amdgcn_image_sample_c_l_2d, Intrinsic::amdgcn_image_sample_c_lz_2d},
				 {Intrinsic::amdgcn_image_sample_c_o_2d, Intrinsic::amdgcn_image_sample_c_b_o_2d, Intrinsic::amdgcn_image_sample_c_l_o_2d, Intrinsic::amdgcn_image_sample_c_lz_o_2d}}};
			Value *t = descriptor(c->getArgOperand(0), 8);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Value *xy = c->getArgOperand(3);
			Type *ft = b.getFloatTy();
			SmallVector<Value *, 12> args = {b.getInt32(1)};
			if (packed)
				args.push_back(b.getInt32(packed));
			if (mode == 1)   // (LLVM's order: offset, bias, compare value, coordinates)
				args.push_back(value);
			if (compare)
				args.push_back(c->getArgOperand(4));
			args.append({b.CreateExtractElement(xy, uint64_t(0)), b.CreateExtractElement(xy, 1)});
			if (mode == 2)
				args.push_back(value);
			args.append({t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			SmallVector<Type *, 5> types = {ft};
			if (mode == 1)
				types.push_back(ft);   // the bias's own overload
			types.append({ft, t->getType(), sm->getType()});
			Value *d = b.CreateIntrinsic(ids[compare][packed != 0][mode], types, args);
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), d, 0), b.getInt8(0), 1);
		} else if (tex.starts_with("read_depth_2d.f32")) {
			// depth2d read (texture, sampler, i32, uint2 or ushort2, int2 offset, lod, i32): its depth as 32_FLOAT (as the sample
			// above) through image_load(_mip). G-buffer depth in Quake II's ray-traced effects (BeauBender/quake2's ref_metal). Without the
			// sampler: (texture, i32, coord, lod, i32), no offset (MetalGraphicsPlayground's SSAO kernels).
			const bool samplerless = !c->getArgOperand(1)->getType()->isPointerTy();
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(samplerless ? 2 : 3);
			Value *offset = samplerless ? ConstantAggregateZero::get(xy->getType()) : c->getArgOperand(4), *lod = c->getArgOperand(samplerless ? 3 : 5);
			Value *x = b.CreateZExt(b.CreateExtractElement(xy, uint64_t(0)), b.getInt32Ty());
			Value *y = b.CreateZExt(b.CreateExtractElement(xy, 1), b.getInt32Ty());
			if (!isa<ConstantAggregateZero>(offset)) {
				x = b.CreateAdd(x, b.CreateSExtOrTrunc(b.CreateExtractElement(offset, uint64_t(0)), b.getInt32Ty()));
				y = b.CreateAdd(y, b.CreateSExtOrTrunc(b.CreateExtractElement(offset, 1), b.getInt32Ty()));
			}
			Value *d = match(lod, m_Zero())
			               ? b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2d, {b.getFloatTy(), b.getInt32Ty(), t->getType()},
			                                   {b.getInt32(1), x, y, t, b.getInt32(0), b.getInt32(0)})
			               : b.CreateIntrinsic(Intrinsic::amdgcn_image_load_mip_2d, {b.getFloatTy(), b.getInt32Ty(), t->getType()},
			                                   {b.getInt32(1), x, y, b.CreateZExtOrTrunc(lod, b.getInt32Ty()), t, b.getInt32(0), b.getInt32(0)});
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), d, 0), b.getInt8(0), 1);
		} else if (tex.starts_with("sample_texture_cube.v4f")) {
			// (texture, sampler, float3 direction, i1 level, float bias or LOD, float min LOD, i32): vkQuake's sky through MoltenVK
			// (2026-11-21). The direction as the hardware's cube coordinates, as Mesa's ac_prepare_cube_coords: the face (cubeid) and
			// s, t = sc, tc / |ma| + 1.5 (cubema is twice the major axis).
			auto *level = dyn_cast<ConstantInt>(c->getArgOperand(3));
			Value *value = c->getArgOperand(4);
			if (!level || !match(c->getArgOperand(5), m_AnyZeroFP()))
				return f->getName().str() + " with a run-time level kind or a minimum LOD is not supported yet";
			Value *t = descriptor(c->getArgOperand(0), 8), *dir = c->getArgOperand(2);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Type *ft = b.getFloatTy(), *v4f = FixedVectorType::get(ft, 4);
			Value *x = b.CreateExtractElement(dir, uint64_t(0)), *y = b.CreateExtractElement(dir, 1), *z = b.CreateExtractElement(dir, 2);
			Value *ma = b.CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, {x, y, z});
			Value *invma = b.CreateFDiv(ConstantFP::get(ft, 1.0), b.CreateUnaryIntrinsic(Intrinsic::fabs, ma));
			Value *bias15 = ConstantFP::get(ft, 1.5);
			Value *sc = b.CreateFAdd(b.CreateFMul(b.CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, {x, y, z}), invma), bias15);
			Value *tc = b.CreateFAdd(b.CreateFMul(b.CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, {x, y, z}), invma), bias15);
			Value *face = b.CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, {x, y, z});
			const bool zero = match(value, m_AnyZeroFP()), lod = level->isOne(), bias = !lod && !zero;
			const bool quads = k.getCallingConv() == CallingConv::AMDGPU_PS;   // implicit LODs need pixel quads (as the 2D case)
			const unsigned mode = !quads && !(lod && !zero) ? 3 : bias ? 1 : lod ? (zero ? 3 : 2) : 0;
			static const Intrinsic::ID ids[4] = {Intrinsic::amdgcn_image_sample_cube, Intrinsic::amdgcn_image_sample_b_cube,
			                                     Intrinsic::amdgcn_image_sample_l_cube, Intrinsic::amdgcn_image_sample_lz_cube};
			SmallVector<Type *, 5> types = {v4f};
			SmallVector<Value *, 10> args = {b.getInt32(15)};
			if (mode == 1) {
				types.push_back(ft);
				args.push_back(value);
			}
			types.push_back(ft);
			args.append({sc, tc, face});
			if (mode == 2)
				args.push_back(value);
			types.append({t->getType(), sm->getType()});
			args.append({t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			Value *texel = b.CreateIntrinsic(ids[mode], types, args);
			if (tex.starts_with("sample_texture_cube.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("gather_texture_2d.v4f")) {
			// (texture, sampler, float2 coord, i1, int2 offset, i32 component, i32): one component of the four texels around the coordinate
			// (MoltenVK's shaders from vkQuake, 2026-11-17), image_gather4 at level 0 with that component's dmask; the order of the four is
			// Metal's and the hardware's alike. A constant offset only.
			auto *offset = dyn_cast<Constant>(c->getArgOperand(4));
			auto *component = dyn_cast<ConstantInt>(c->getArgOperand(5));
			if (!offset || !component || component->getZExtValue() > 3)
				return f->getName().str() + " with a run-time offset or component is not supported yet";
			uint32_t packed = 0;
			for (unsigned k = 0; k < 2; k++)
				if (auto *e = dyn_cast_or_null<ConstantInt>(offset->getAggregateElement(k)))
					packed |= (static_cast<uint32_t>(e->getSExtValue()) & 0x3f) << (8 * k);
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(2);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			SmallVector<Value *, 10> args = {b.getInt32(1u << component->getZExtValue())};
			if (packed)
				args.push_back(b.getInt32(packed));
			args.append({b.CreateExtractElement(xy, uint64_t(0)), b.CreateExtractElement(xy, 1), t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			Value *texel = b.CreateIntrinsic(packed ? Intrinsic::amdgcn_image_gather4_lz_o_2d : Intrinsic::amdgcn_image_gather4_lz_2d,
			                                 {v4f, b.getFloatTy(), t->getType(), sm->getType()}, args);
			if (tex.starts_with("gather_texture_2d.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("sample_texture_2d_array.v4f")) {
			// (texture, sampler, float2 coord, uint slice, i1, int2 offset, i1 level, float bias or LOD, float min LOD, i32): kitty's glyph
			// atlas through AppleMetalOpenGLRenderer (2026-11-12). The slice is the T#'s array layer, given as a float. An explicit LOD as
			// given; else, in a fragment shader, from its quads (with the bias), elsewhere level 0.
			const bool half = tex.starts_with("sample_texture_2d_array.v4f16");
			// (older AIR, Cubase's text: eight operands, without the minimum LOD and the i32)
			auto *level = c->arg_size() >= 8 ? dyn_cast<ConstantInt>(c->getArgOperand(6)) : nullptr;
			Value *minLod = c->arg_size() >= 10 ? c->getArgOperand(8) : nullptr;
			if (!level || !match(c->getArgOperand(5), m_Zero()) || (minLod && !match(minLod, m_AnyZeroFP())))
				return f->getName().str() + " with an offset, a minimum LOD or a run-time level flag is not supported yet";
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(2), *value = c->getArgOperand(7);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Value *x = b.CreateExtractElement(xy, uint64_t(0)), *y = b.CreateExtractElement(xy, 1);
			Value *slice = b.CreateUIToFP(c->getArgOperand(3), b.getFloatTy());
			const bool zero = match(value, m_AnyZeroFP()), lod = level->isOne(), quads = k.getCallingConv() == CallingConv::AMDGPU_PS;
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4), *fl = b.getFloatTy();
			Value *texel;
			if (lod && !zero)
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_l_2darray, {v4f, fl, t->getType(), sm->getType()},
				                          {b.getInt32(15), x, y, slice, value, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			else if (!lod && !zero && quads)
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_b_2darray, {v4f, fl, fl, t->getType(), sm->getType()},
				                          {b.getInt32(15), value, x, y, slice, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			else if (!lod && quads)
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_2darray, {v4f, fl, t->getType(), sm->getType()},
				                          {b.getInt32(15), x, y, slice, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			else
				texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_lz_2darray, {v4f, fl, t->getType(), sm->getType()},
				                          {b.getInt32(15), x, y, slice, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			if (half)
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			// {texel, i8 0}, or older AIR's texel alone
			r = c->getType()->isStructTy() ? b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1) : texel;
		} else if (tex.starts_with("read_texture_2d_array.v4f")) {
			// (texture, uint2 or int2 coord, uint slice, uint lod, i32): kitty's glyph atlas read at the texel (2026-11-12).
			const bool half = tex.starts_with("read_texture_2d_array.v4f16");
			if (c->arg_size() < 4)
				return f->getName().str() + " without a slice and a LOD is not known";
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(1), *lod = c->getArgOperand(3);
			Value *x = b.CreateZExtOrTrunc(b.CreateExtractElement(xy, uint64_t(0)), b.getInt32Ty());
			Value *y = b.CreateZExtOrTrunc(b.CreateExtractElement(xy, 1), b.getInt32Ty());
			Value *slice = b.CreateZExtOrTrunc(c->getArgOperand(2), b.getInt32Ty());
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel = match(lod, m_Zero())
			                   ? b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2darray, {v4f, b.getInt32Ty(), t->getType()},
			                                       {b.getInt32(15), x, y, slice, t, b.getInt32(0), b.getInt32(0)})
			                   : b.CreateIntrinsic(Intrinsic::amdgcn_image_load_mip_2darray, {v4f, b.getInt32Ty(), t->getType()},
			                                       {b.getInt32(15), x, y, slice, b.CreateZExtOrTrunc(lod, b.getInt32Ty()), t, b.getInt32(0), b.getInt32(0)});
			if (half)
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("atomic_fetch_") && tex.contains("_explicit_texture_2d.")) {
			// atomic_fetch_<op>_explicit_texture_2d.{u,s}.v4i32 (texture, uint2 coord, uint4 value, i32 order, i32 scope): the old texel in x
			// (vkQuake's depth pyramid ORs into an R32Uint image); the image atomic on .x, relaxed as Metal's texture atomics are
			static const std::map<std::string, Intrinsic::ID> ops = {
				{"or", Intrinsic::amdgcn_image_atomic_or_2d}, {"and", Intrinsic::amdgcn_image_atomic_and_2d},
				{"xor", Intrinsic::amdgcn_image_atomic_xor_2d}, {"add", Intrinsic::amdgcn_image_atomic_add_2d},
				{"sub", Intrinsic::amdgcn_image_atomic_sub_2d}, {"min", Intrinsic::amdgcn_image_atomic_umin_2d},
				{"max", Intrinsic::amdgcn_image_atomic_umax_2d}};
			StringRef rest = tex.drop_front(strlen("atomic_fetch_"));
			const std::string what = rest.substr(0, rest.find("_explicit")).str();
			auto it = ops.find(what);
			auto *vt = dyn_cast<FixedVectorType>(c->getType());
			if (it == ops.end() || !vt || !vt->getElementType()->isIntegerTy(32) || c->arg_size() < 3)
				return f->getName().str() + " is not supported yet";
			Intrinsic::ID op = it->second;   // an int texture's min and max are signed
			if (tex.contains("_texture_2d.s.") && what == "min")
				op = Intrinsic::amdgcn_image_atomic_smin_2d;
			else if (tex.contains("_texture_2d.s.") && what == "max")
				op = Intrinsic::amdgcn_image_atomic_smax_2d;
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(1);
			Value *x = b.CreateZExtOrTrunc(b.CreateExtractElement(xy, uint64_t(0)), b.getInt32Ty());
			Value *y = b.CreateZExtOrTrunc(b.CreateExtractElement(xy, 1), b.getInt32Ty());
			Value *value = b.CreateExtractElement(c->getArgOperand(2), uint64_t(0));
			Value *old = b.CreateIntrinsic(op, {b.getInt32Ty(), b.getInt32Ty(), t->getType()},
			                               {value, x, y, t, b.getInt32(0), b.getInt32(0)});
			r = b.CreateInsertElement(PoisonValue::get(vt), old, uint64_t(0));
		} else if (tex.starts_with("read_texture_buffer_1d.v4f")) {
			// (texture, uint coord, i32): a texture made from a buffer (vkQuake's palette, Sublime Text's GL buffer textures), which the host
			// gives as one linear row, or as rows of 16384 texels when longer (a T# holds at most 16384): texel x at (x % 16384, x / 16384)
			const bool half = tex.starts_with("read_texture_buffer_1d.v4f16");
			Value *t = descriptor(c->getArgOperand(0), 8), *x = b.CreateZExtOrTrunc(c->getArgOperand(1), b.getInt32Ty());
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2d, {v4f, b.getInt32Ty(), t->getType()},
			                                 {b.getInt32(15), b.CreateAnd(x, 16383), b.CreateLShr(x, 14), t, b.getInt32(0), b.getInt32(0)});
			if (half)
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("sample_texture_1d_array.v4f")) {
			// (texture, sampler, float x, uint slice, i1, int offset, i1 level, float bias or LOD, float min LOD, i32): WindowServer's colour
			// lookup tables, one mip level, so read at level 0 (sample_lz); the slice is the T#'s array layer, given as a float.
			const bool half = tex.starts_with("sample_texture_1d_array.v4f16");
			if (c->arg_size() < 10 || !match(c->getArgOperand(5), m_Zero()) || !match(c->getArgOperand(8), m_AnyZeroFP()))
				return f->getName().str() + " with an offset or a minimum LOD is not supported yet";
			Value *t = descriptor(c->getArgOperand(0), 8);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Value *slice = b.CreateUIToFP(c->getArgOperand(3), b.getFloatTy());
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_sample_lz_1darray, {v4f, b.getFloatTy(), t->getType(), sm->getType()},
			                                 {b.getInt32(15), c->getArgOperand(2), slice, t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			if (half)
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("write_texture_2d.v4f") || tex.starts_with("write_texture_2d.u.v4i") || tex.starts_with("write_texture_2d.s.v4i")) {
			// (texture, uint2 or ushort2 coord, float4 or half4 value, i32 lod, i32 access); or an integer texture's uint4 / int4 (or
			// ushort4 / short4) value (VideoToolbox's VTMTSComputeFunction), stored as the bits of a float4 as Mesa does: the intrinsic
			// takes float data only, and the T#'s integer format writes the bits
			if (c->arg_size() < 4 || !match(c->getArgOperand(3), m_Zero()))
				return f->getName().str() + " to a level other than 0 is not supported yet";
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(1), *value = c->getArgOperand(2);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4), *v4i = FixedVectorType::get(b.getInt32Ty(), 4);
			if (!tex.starts_with("write_texture_2d.v4f"))
				value = b.CreateBitCast(b.CreateIntCast(value, v4i, tex.starts_with("write_texture_2d.s")), v4f);
			else if (value->getType() != v4f)
				value = b.CreateFPExt(value, v4f);
			Value *x = b.CreateZExt(b.CreateExtractElement(xy, uint64_t(0)), b.getInt32Ty()), *y = b.CreateZExt(b.CreateExtractElement(xy, 1), b.getInt32Ty());
			b.CreateIntrinsic(Intrinsic::amdgcn_image_store_2d, {v4f, b.getInt32Ty(), t->getType()}, {value, b.getInt32(15), x, y, t, b.getInt32(0), b.getInt32(0)});
		} else if (tex.starts_with("write_texture_3d.") || tex.starts_with("write_texture_2d_array.")) {
			// 3D: (texture, uint3 coord, value, i32 lod, i32 access); 2D array: (texture, uint2 coord, uint slice, value, i32 lod, i32
			// access). Float or half texels, or an integer texture's bits as write_texture_2d's. Unity's post-processing bakes its colour
			// grading table into a 3D texture this way (Daggerfall Unity, 2026-11-22). Level 0 only.
			const bool array = tex.starts_with("write_texture_2d_array.");
			const unsigned vi = array ? 3 : 2;
			if (c->arg_size() < vi + 2 || !match(c->getArgOperand(vi + 1), m_Zero()))
				return f->getName().str() + " to a level other than 0 is not supported yet";
			const StringRef kind = tex.drop_front(array ? strlen("write_texture_2d_array.") : strlen("write_texture_3d."));
			Value *t = descriptor(c->getArgOperand(0), 8), *xyz = c->getArgOperand(1), *value = c->getArgOperand(vi);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4), *v4i = FixedVectorType::get(b.getInt32Ty(), 4);
			if (!kind.starts_with("v4f"))
				value = b.CreateBitCast(b.CreateIntCast(value, v4i, kind.starts_with("s.")), v4f);
			else if (value->getType() != v4f)
				value = b.CreateFPExt(value, v4f);
			auto coord = [&](Value *v, unsigned i) { return b.CreateZExtOrTrunc(b.CreateExtractElement(v, uint64_t(i)), b.getInt32Ty()); };
			Value *z = array ? b.CreateZExtOrTrunc(c->getArgOperand(2), b.getInt32Ty()) : coord(xyz, 2);
			b.CreateIntrinsic(array ? Intrinsic::amdgcn_image_store_2darray : Intrinsic::amdgcn_image_store_3d, {v4f, b.getInt32Ty(), t->getType()},
			                  {value, b.getInt32(15), coord(xyz, 0), coord(xyz, 1), z, t, b.getInt32(0), b.getInt32(0)});
		} else if (tex.starts_with("read_texture_3d.v4f")) {
			// (texture, sampler, uint3 coord, int3 offset, i32 lod, i32): the offset added, a level other than 0 through image_load_mip
			Value *t = descriptor(c->getArgOperand(0), 8), *xyz = c->getArgOperand(2), *offset = c->getArgOperand(3), *lod = c->getArgOperand(4);
			Value *v[3];
			for (unsigned i = 0; i < 3; i++) {
				v[i] = b.CreateZExtOrTrunc(b.CreateExtractElement(xyz, uint64_t(i)), b.getInt32Ty());
				if (!isa<ConstantAggregateZero>(offset))
					v[i] = b.CreateAdd(v[i], b.CreateSExtOrTrunc(b.CreateExtractElement(offset, uint64_t(i)), b.getInt32Ty()));
			}
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			Value *texel = match(lod, m_Zero())
			                   ? b.CreateIntrinsic(Intrinsic::amdgcn_image_load_3d, {v4f, b.getInt32Ty(), t->getType()},
			                                       {b.getInt32(15), v[0], v[1], v[2], t, b.getInt32(0), b.getInt32(0)})
			                   : b.CreateIntrinsic(Intrinsic::amdgcn_image_load_mip_3d, {v4f, b.getInt32Ty(), t->getType()},
			                                       {b.getInt32(15), v[0], v[1], v[2], b.CreateZExtOrTrunc(lod, b.getInt32Ty()), t, b.getInt32(0),
			                                        b.getInt32(0)});
			if (tex.starts_with("read_texture_3d.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (tex.starts_with("gather_texture_2d_array.v4f")) {
			// (texture, sampler, float2 coord, uint slice, i1, int2 offset, i32 component, i32): as gather_texture_2d's, the slice the
			// T#'s array layer as a float
			auto *offset = dyn_cast<Constant>(c->getArgOperand(5));
			auto *component = dyn_cast<ConstantInt>(c->getArgOperand(6));
			if (!offset || !component || component->getZExtValue() > 3)
				return f->getName().str() + " with a run-time offset or component is not supported yet";
			uint32_t packed = 0;
			for (unsigned i = 0; i < 2; i++)
				if (auto *e = dyn_cast_or_null<ConstantInt>(offset->getAggregateElement(i)))
					packed |= (static_cast<uint32_t>(e->getSExtValue()) & 0x3f) << (8 * i);
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(2);
			Value *sm = constSampler.count(c) ? static_cast<Value *>(constSampler[c]) : descriptor(c->getArgOperand(1), 4);
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			SmallVector<Value *, 11> args = {b.getInt32(1u << component->getZExtValue())};
			if (packed)
				args.push_back(b.getInt32(packed));
			args.append({b.CreateExtractElement(xy, uint64_t(0)), b.CreateExtractElement(xy, 1), b.CreateUIToFP(c->getArgOperand(3), b.getFloatTy()),
			             t, sm, b.getFalse(), b.getInt32(0), b.getInt32(0)});
			Value *texel = b.CreateIntrinsic(packed ? Intrinsic::amdgcn_image_gather4_lz_o_2darray : Intrinsic::amdgcn_image_gather4_lz_2darray,
			                                 {v4f, b.getFloatTy(), t->getType(), sm->getType()}, args);
			if (tex.starts_with("gather_texture_2d_array.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (name == "fence_texture_2d") {   // the thread's texture writes seen by its reads after: their memory, in order
			b.CreateFence(AtomicOrdering::AcquireRelease, ctx.getOrInsertSyncScopeID("agent"));
		} else if (name.starts_with("get_sample_position.")) {   // ponytail: the pixel's centre, as [[sample_id]] is 0 (see there)
			r = ConstantFP::get(c->getType(), 0.5);
		} else if (name == "discard_fragment") {   // fragment only: the lane is demoted, its export and writes are dropped
			b.CreateIntrinsic(Intrinsic::amdgcn_wqm_demote, {}, {b.getFalse()});
		} else if (name == "get_read_sampler") {
			r = PoisonValue::get(c->getType());   // read_texture's sampler operand: unused
		} else if (op.starts_with("unpack.") || op.starts_with("pack.")) {
			r = packOp(b, op, c);
			if (!r)
				return f->getName().str() + " is not supported yet";
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
		} else if (name == "simdgroup.barrier") {
			// (the same): a SIMD group is one wave, which runs in step: only the fence, at the wave's scope (device memory: the agent's).
			// CoreImage's 4:2:0 kernels (ci_writeTG_420_*) were not translated without it.
			const bool device = cast<ConstantInt>(c->getArgOperand(0))->getZExtValue() & 1;
			b.CreateFence(AtomicOrdering::AcquireRelease, ctx.getOrInsertSyncScopeID(device ? "agent" : "wavefront"));
		} else if (name.starts_with("convert.")) {   // convert.<to kind>.<to type>.<from kind>.<from type>; kinds f, s, u
			SmallVector<StringRef, 5> k;
			name.split(k, '.');
			if (k.size() != 5)
				return f->getName().str() + " is not supported yet";
			Value *v = c->getArgOperand(0);
			Type *to = c->getType();
			// to bool: x != 0, as Metal's bool(x) (RenderBox's even-odd resolve converts a winding of -1: saturated, it was false)
			if (to->isIntOrIntVectorTy(1) && k[1] != "f")
				r = k[3] == "f" ? b.CreateFCmpUNE(v, Constant::getNullValue(v->getType())) : b.CreateICmpNE(v, Constant::getNullValue(v->getType()));
			else if (k[1] == "f" && k[3] == "f")
				r = b.CreateFPCast(v, to);
			else if (k[3] == "f")   // float to integer: saturating
				r = b.CreateIntrinsic(k[1] == "s" ? Intrinsic::fptosi_sat : Intrinsic::fptoui_sat, {to, v->getType()}, {v});
			else if (k[1] == "f")
				r = k[3] == "s" ? b.CreateSIToFP(v, to) : b.CreateUIToFP(v, to);
			else
				r = b.CreateIntCast(v, to, k[3] == "s");
		} else if (name.consume_front("atomic.global.") || name.consume_front("atomic.local.")) {
			// (pointer, value, memory order, scope, volatile), a load without the value: Metal's are relaxed; device or threadgroup scope by
			// the memory.
			const SyncScope::ID scope = ctx.getOrInsertSyncScopeID(f->getName().contains(".local.") ? "workgroup" : "agent");
			if (name.starts_with("load.")) {   // (Quake II's water lists)
				LoadInst *l = b.CreateAlignedLoad(c->getType(), c->getArgOperand(0), Align(c->getType()->getPrimitiveSizeInBits() / 8));
				l->setAtomic(AtomicOrdering::Monotonic, scope);
				r = l;
			} else if (name.starts_with("store.")) {
				Value *v = c->getArgOperand(1);
				b.CreateAlignedStore(v, c->getArgOperand(0), Align(v->getType()->getPrimitiveSizeInBits() / 8))->setAtomic(AtomicOrdering::Monotonic, scope);
			} else {
				auto it = rmw.find(name.rsplit('.').first.str());   // "add.u" -> "add"/"min.u"
				if (it == rmw.end())
					it = rmw.find(name.rsplit('.').first.rsplit('.').first.str());
				if (it == rmw.end())
					return f->getName().str() + " is not supported yet";
				r = b.CreateAtomicRMW(it->second, c->getArgOperand(0), c->getArgOperand(1), MaybeAlign(), AtomicOrdering::Monotonic, scope);
			}
		} else if (same.count(op.str())) {
			SmallVector<Value *, 3> args;   // one to three operands of the call's type; AIR's i1 flags have no counterpart
			for (Value *v : c->args())
				if (!v->getType()->isIntegerTy(1))
					args.push_back(v);
			r = b.CreateIntrinsic(same.at(op.str()), {c->getType()}, args);
		} else if (op == "get_num_samples") {   // ANGLE's (WebGL): ponytail: 1, as [[sample_id]] is 0 (no per-sample shading); the pass's count when that comes
			r = ConstantInt::get(c->getType(), 1);
		} else if (op == "acos" || op == "asin") {
			// acos(|x|) = sqrt(1 - |x|) * p(|x|), Abramowitz and Stegun 4.4.46 (error 2e-8), pi minus it for x < 0; asin = pi/2 - acos (Dawn's)
			static const double a[] = {1.5707963050, -0.2145988016, 0.0889789874, -0.0501743046, 0.0308918810, -0.0170881256, 0.0066700901, -0.0012624911};
			Type *t = c->getType();
			Value *x = c->getArgOperand(0), *ax = b.CreateUnaryIntrinsic(Intrinsic::fabs, x), *p = ConstantFP::get(t, a[7]);
			for (int k = 6; k >= 0; k--)
				p = b.CreateFAdd(b.CreateFMul(p, ax), ConstantFP::get(t, a[k]));
			Value *pos = b.CreateFMul(b.CreateUnaryIntrinsic(Intrinsic::sqrt, b.CreateFSub(ConstantFP::get(t, 1.0), ax)), p);
			r = b.CreateSelect(b.CreateFCmpOLT(x, ConstantFP::get(t, 0.0)), b.CreateFSub(ConstantFP::get(t, M_PI), pos), pos);
			if (op == "asin")
				r = b.CreateFSub(ConstantFP::get(t, M_PI / 2), r);
		} else if (op == "ldexp") {   // (x, n): x * 2^n (Dawn's, Chrome's compositor)
			r = b.CreateIntrinsic(Intrinsic::ldexp, {c->getType(), c->getArgOperand(1)->getType()}, {c->getArgOperand(0), c->getArgOperand(1)});
		} else if (op == "clz" || op == "ctz") {   // (x, is zero poison)
			r = b.CreateIntrinsic(op == "clz" ? Intrinsic::ctlz : Intrinsic::cttz, {c->getType()}, {c->getArgOperand(0), b.getFalse()});
		} else if ((name.starts_with("get_width_") || name.starts_with("get_height_")) &&
		           (name.contains("texture_2d") || name.contains("depth_2d") || name.contains("texture_cube") || name.contains("depth_cube"))) {
			// (texture, lod), a depth or cube texture's too (the same fields): from the T# the host wrote (src/pvgpu.cpp): width - 1 in word1 [31:30]
			// and word2 [13:0] << 2, height - 1 in word2 [27:14]; then >> lod, at least 1.
			Value *t = descriptor(c->getArgOperand(0), 8), *w1 = b.CreateExtractElement(t, 1), *w2 = b.CreateExtractElement(t, 2);
			Value *size = name.starts_with("get_width") ? b.CreateOr(b.CreateLShr(w1, 30), b.CreateShl(b.CreateAnd(w2, 0x3fff), 2))
			                                            : b.CreateAnd(b.CreateLShr(w2, 14), 0x3fff);
			// a multisampled texture's (get_width_texture_2d_ms) has no LOD: level 0
			Value *lod = c->arg_size() > 1 ? c->getArgOperand(1) : b.getInt32(0);
			r = b.CreateIntrinsic(Intrinsic::umax, {b.getInt32Ty()}, {b.CreateLShr(b.CreateAdd(size, b.getInt32(1)), lod), b.getInt32(1)});
		} else if (name.starts_with("get_num_mip_levels_")) {
			// (texture): LAST_LEVEL (word3 [19:15]) + 1. ponytail: BASE_LEVEL taken as 0, as the T#s the host and the bundle write
			// (a view of a later level is an image of its own); subtract it if views ever start at a level in the T#.
			Value *w3 = b.CreateExtractElement(descriptor(c->getArgOperand(0), 8), 3);
			r = b.CreateAdd(b.CreateAnd(b.CreateLShr(w3, 15), 0x1f), b.getInt32(1));
		} else if (name.starts_with("get_array_size_texture_1d_array") || name.starts_with("get_array_size_texture_2d_array")) {
			// (texture): the T#'s layers - 1 in word4 [13:0] (pvdraw.hpp linearArray1d / linearArray2d), plus one (Quake II's water)
			Value *t = descriptor(c->getArgOperand(0), 8);
			r = b.CreateAdd(b.CreateAnd(b.CreateExtractElement(t, 4), 0x3fff), b.getInt32(1));
		} else if (name.starts_with("get_num_samples_texture_2d_ms")) {
			// (texture): 1 << LAST_LEVEL, which holds a multisampled image's log2 of its samples (word3 [19:15], as Mesa's
			// ac_build_gfx12_texture_descriptor)
			Value *w3 = b.CreateExtractElement(descriptor(c->getArgOperand(0), 8), 3);
			r = b.CreateShl(b.getInt32(1), b.CreateAnd(b.CreateLShr(w3, 15), 0x1f));
		} else if (tex.starts_with("read_texture_2d_ms.v4f") || tex.starts_with("read_texture_2d_ms.u.v4i") || tex.starts_with("read_texture_2d_ms.s.v4i")) {
			// (texture, sampler, uint2 coord, uint sample, i32), or without the sampler (MoltenVK's, 2026-11-17): that sample of the texel,
			// float or half (the resolve the host runs for a pass's multisampled attachment, and shaders reading a texture2d_ms); an integer
			// texture's as the bits (vkQuake's entity mask), as read_texture_2d's
			Type *v4f = FixedVectorType::get(b.getFloatTy(), 4);
			const unsigned at = c->getArgOperand(1)->getType()->isVectorTy() ? 1 : 2;   // the coordinate's operand
			Value *t = descriptor(c->getArgOperand(0), 8), *xy = c->getArgOperand(at);
			Value *x = b.CreateZExt(b.CreateExtractElement(xy, uint64_t(0)), b.getInt32Ty()), *y = b.CreateZExt(b.CreateExtractElement(xy, 1), b.getInt32Ty());
			Value *texel = b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2dmsaa, {v4f, b.getInt32Ty(), t->getType()},
			                                 {b.getInt32(15), x, y, b.CreateZExtOrTrunc(c->getArgOperand(at + 1), b.getInt32Ty()), t, b.getInt32(0), b.getInt32(0)});
			if (tex.starts_with("read_texture_2d_ms.v4f16"))
				texel = b.CreateFPTrunc(texel, FixedVectorType::get(b.getHalfTy(), 4));
			else if (!tex.starts_with("read_texture_2d_ms.v4f"))
				texel = b.CreateIntCast(b.CreateBitCast(texel, FixedVectorType::get(b.getInt32Ty(), 4)),
				                        cast<StructType>(c->getType())->getElementType(0), tex.starts_with("read_texture_2d_ms.s"));
			r = b.CreateInsertValue(b.CreateInsertValue(PoisonValue::get(c->getType()), texel, 0), b.getInt8(0), 1);   // {texel, i8 0}
		} else if (auto cl = clampOps.find(op.str()); cl != clampOps.end()) {   // (x, low, high): min(max(x, low), high)
			Value *lo = b.CreateIntrinsic(cl->second.first, {c->getType()}, {c->getArgOperand(0), c->getArgOperand(1)});
			r = b.CreateIntrinsic(cl->second.second, {c->getType()}, {lo, c->getArgOperand(2)});
		} else if (op == "saturate") {   // min(max(x, 0), 1)
			r = b.CreateIntrinsic(Intrinsic::minnum, {c->getType()},
			                      {b.CreateIntrinsic(Intrinsic::maxnum, {c->getType()}, {c->getArgOperand(0), ConstantFP::get(c->getType(), 0.0)}),
			                       ConstantFP::get(c->getType(), 1.0)});
		} else if (op == "sign") {   // 1, -1 or 0 (NaN: 0)
			Value *x = c->getArgOperand(0), *zero = ConstantFP::get(c->getType(), 0.0);
			r = b.CreateSelect(b.CreateFCmpOGT(x, zero), ConstantFP::get(c->getType(), 1.0),
			                   b.CreateSelect(b.CreateFCmpOLT(x, zero), ConstantFP::get(c->getType(), -1.0), zero));
		} else if (op == "any" || op == "all") {   // of a bool vector
			r = b.CreateIntrinsic(op == "any" ? Intrinsic::vector_reduce_or : Intrinsic::vector_reduce_and, {c->getArgOperand(0)->getType()}, {c->getArgOperand(0)});
		} else if (op == "dfdx" || op == "dfdy" || op == "fwidth") {
			// Fine derivatives inside the pixel quad (lanes 0 1 / 2 3 in raster order): x = lane | 1 - lane & ~1, y = lane | 2 - lane & ~2,
			// each component through DPP quad_perm in whole-quad mode; fwidth = |dfdx| + |dfdy|.
			auto quadDiff = [&](Value *x, bool dy) {
				Type *t = x->getType();
				Value *f = t->isHalfTy() ? b.CreateFPExt(x, b.getFloatTy()) : x;
				auto lane = [&](uint32_t perm) {
					return b.CreateBitCast(b.CreateIntrinsic(Intrinsic::amdgcn_update_dpp, {b.getInt32Ty()},
					                                          {PoisonValue::get(b.getInt32Ty()), b.CreateBitCast(f, b.getInt32Ty()), b.getInt32(perm),
					                                           b.getInt32(15), b.getInt32(15), b.getFalse()}),
					                       b.getFloatTy());
				};
				Value *d = b.CreateFSub(lane(dy ? 0xee : 0xf5), lane(dy ? 0x44 : 0xa0));   // quad_perm (2,3,2,3)-(0,1,0,1), (1,1,3,3)-(0,0,2,2)
				d = b.CreateIntrinsic(Intrinsic::amdgcn_wqm, {b.getFloatTy()}, {d});
				return t->isHalfTy() ? b.CreateFPTrunc(d, t) : d;
			};
			Value *x = c->getArgOperand(0);
			auto *vt = dyn_cast<FixedVectorType>(x->getType());
			r = vt ? PoisonValue::get(vt) : nullptr;
			for (unsigned k = 0; k < (vt ? vt->getNumElements() : 1); k++) {
				Value *e = vt ? b.CreateExtractElement(x, k) : x, *v;
				if (op == "fwidth")
					v = b.CreateFAdd(b.CreateIntrinsic(Intrinsic::fabs, {e->getType()}, {quadDiff(e, false)}),
					                 b.CreateIntrinsic(Intrinsic::fabs, {e->getType()}, {quadDiff(e, true)}));
				else
					v = quadDiff(e, op == "dfdy");
				r = vt ? b.CreateInsertElement(r, v, k) : v;
			}
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
		} else if (op == "sincos") {   // (x, ptr cos): the sine returned, the cosine stored (the Sequoia wallpaper's drawVertex)
			b.CreateStore(b.CreateIntrinsic(Intrinsic::cos, {c->getType()}, {c->getArgOperand(0)}), c->getArgOperand(1));
			r = b.CreateIntrinsic(Intrinsic::sin, {c->getType()}, {c->getArgOperand(0)});
		} else if (op == "pow" || op == "powr") {   // fast math's x^y = 2^(y log2 x)
			r = b.CreateIntrinsic(Intrinsic::exp2, {c->getType()},
			                      {b.CreateFMul(c->getArgOperand(1), b.CreateIntrinsic(Intrinsic::log2, {c->getType()}, {c->getArgOperand(0)}))});
		} else if (op == "fmax3" || op == "fmin3") {   // (a, b, c): the largest or smallest of the three
			const Intrinsic::ID id = op == "fmax3" ? Intrinsic::maxnum : Intrinsic::minnum;
			Type *t = c->getType();
			r = b.CreateIntrinsic(id, {t}, {b.CreateIntrinsic(id, {t}, {c->getArgOperand(0), c->getArgOperand(1)}), c->getArgOperand(2)});
		} else if (op == "fmedian3") {   // max(min(a, b), min(max(a, b), c))
			Type *t = c->getType();
			Value *a = c->getArgOperand(0), *v = c->getArgOperand(1);
			Value *hi = b.CreateIntrinsic(Intrinsic::minnum, {t}, {b.CreateIntrinsic(Intrinsic::maxnum, {t}, {a, v}), c->getArgOperand(2)});
			r = b.CreateIntrinsic(Intrinsic::maxnum, {t}, {b.CreateIntrinsic(Intrinsic::minnum, {t}, {a, v}), hi});
		} else if (op == "fract") {   // x - floor(x), below 1 (Metal's: at most 0x1.fffffep-1)
			Type *t = c->getType();
			Value *x = c->getArgOperand(0);
			r = b.CreateFSub(x, b.CreateIntrinsic(Intrinsic::floor, {t}, {x}));
			r = b.CreateIntrinsic(Intrinsic::minnum, {t}, {r, ConstantFP::get(t, t->getScalarType()->isHalfTy() ? 0.99951171875 : 0.99999994039535522)});
		} else if (op == "atan2" || op == "atan") {
			// atan(min / max) of |y|, |x| as a polynomial (error under 1e-5), then moved to its octant and quadrant; atan(x) is atan2(x, 1)
			Type *t = c->getType();
			Value *y = c->getArgOperand(0), *x = op == "atan" ? ConstantFP::get(t, 1.0) : c->getArgOperand(1);
			Value *ax = b.CreateIntrinsic(Intrinsic::fabs, {t}, {x}), *ay = b.CreateIntrinsic(Intrinsic::fabs, {t}, {y});
			Value *hi = b.CreateIntrinsic(Intrinsic::maxnum, {t}, {ax, ay}), *lo = b.CreateIntrinsic(Intrinsic::minnum, {t}, {ax, ay});
			Value *zero = ConstantFP::get(t, 0.0);
			Value *a = b.CreateSelect(b.CreateFCmpOEQ(hi, zero), zero, b.CreateFDiv(lo, hi));
			Value *sq = b.CreateFMul(a, a), *p = ConstantFP::get(t, -0.0464964749);

			p = b.CreateFAdd(b.CreateFMul(p, sq), ConstantFP::get(t, 0.15931422));
			p = b.CreateFAdd(b.CreateFMul(p, sq), ConstantFP::get(t, -0.327622764));
			r = b.CreateFAdd(b.CreateFMul(b.CreateFMul(p, sq), a), a);
			r = b.CreateSelect(b.CreateFCmpOGT(ay, ax), b.CreateFSub(ConstantFP::get(t, 1.5707963267948966), r), r);
			r = b.CreateSelect(b.CreateFCmpOLT(x, zero), b.CreateFSub(ConstantFP::get(t, 3.141592653589793), r), r);
			r = b.CreateSelect(b.CreateFCmpOLT(y, zero), b.CreateFNeg(r), r);
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
// A small local array read or written at an index known only at run time (SPIR-V's through MoltenVK: a matrix's column, an array of float3s):
// each access made into one at every constant index, the right one chosen by selects (a store: each element written with itself or the
// value), so that SROA keeps the array in registers; AMDGPU's promotion leaves such arrays in scratch (padded float3s, a matrix's columns).
// Arrays of at most 16 elements, accessed by plain loads and stores only.
static void constantIndices(Function &f) {
	SmallVector<GetElementPtrInst *, 16> geps;
	for (Instruction &i : instructions(f))
		if (auto *g = dyn_cast<GetElementPtrInst>(&i); g && isa<AllocaInst>(g->getPointerOperand()->stripPointerCasts()))
			geps.push_back(g);
	const DataLayout &dl = f.getParent()->getDataLayout();
	for (GetElementPtrInst *g : geps) {
		unsigned variable = 0, at = 0;
		for (unsigned k = 1; k < g->getNumOperands(); k++)
			if (!isa<Constant>(g->getOperand(k))) {
				variable++;
				at = k;
			}
		if (variable != 1)
			continue;
		// the elements: of the array the index selects in, or (O2's canonical form, `gep [16 x i8], %array, %j`) the alloca's size over the
		// stride of the first index, the GEP on the alloca itself
		uint64_t elements = 0;
		if (at >= 2) {
			SmallVector<Value *, 4> before(g->op_begin() + 2, g->op_begin() + at);
			if (auto *array = dyn_cast_or_null<ArrayType>(GetElementPtrInst::getIndexedType(g->getSourceElementType(), before));
			    array && isa<Constant>(g->getOperand(1)))
				elements = array->getNumElements();
		} else if (auto *a = dyn_cast<AllocaInst>(g->getPointerOperand()); a && a->getAllocationSize(dl)) {
			const uint64_t stride = dl.getTypeAllocSize(g->getSourceElementType()).getFixedValue();
			elements = stride ? a->getAllocationSize(dl)->getFixedValue() / stride : 0;
		}
		// the accesses: plain loads and stores of the element, or of a part of it at a constant offset (a matrix's column)
		auto direct = [](User *u, Value *p) {
			return (isa<LoadInst>(u) && cast<LoadInst>(u)->isSimple()) ||
			       (isa<StoreInst>(u) && cast<StoreInst>(u)->isSimple() && cast<StoreInst>(u)->getPointerOperand() == p);
		};
		SmallVector<std::pair<Instruction *, uint64_t>, 8> accesses;   // the load or store, the offset in the element
		bool plain = elements && elements <= 16;
		for (User *u : g->users()) {
			auto *part = dyn_cast<GetElementPtrInst>(u);
			if (direct(u, g))
				accesses.push_back({cast<Instruction>(u), 0});
			else if (part && part->getSourceElementType()->isIntegerTy(8) && part->getNumIndices() == 1 && isa<ConstantInt>(part->getOperand(1)) &&
			         all_of(part->users(), [&](User *v) { return direct(v, part); }))
				for (User *v : part->users())
					accesses.push_back({cast<Instruction>(v), cast<ConstantInt>(part->getOperand(1))->getZExtValue()});
			else
				plain = false;
		}
		if (!plain)
			continue;
		Value *index = g->getOperand(at);
		for (auto [i, offset] : accesses) {
			IRBuilder<> b(i);
			auto element = [&](uint64_t k) {
				SmallVector<Value *, 6> indices(g->op_begin() + 1, g->op_end());
				indices[at - 1] = ConstantInt::get(index->getType(), k);
				Value *p = b.CreateInBoundsGEP(g->getSourceElementType(), g->getPointerOperand(), indices);
				return offset ? b.CreateInBoundsGEP(b.getInt8Ty(), p, b.getInt64(offset)) : p;
			};
			if (auto *load = dyn_cast<LoadInst>(i)) {
				Value *v = b.CreateAlignedLoad(load->getType(), element(0), load->getAlign());
				for (uint64_t k = 1; k < elements; k++)
					v = b.CreateSelect(b.CreateICmpEQ(index, ConstantInt::get(index->getType(), k)),
					                   b.CreateAlignedLoad(load->getType(), element(k), load->getAlign()), v);
				load->replaceAllUsesWith(v);
			} else {
				auto *store = cast<StoreInst>(i);
				Value *value = store->getValueOperand();
				for (uint64_t k = 0; k < elements; k++) {
					Value *p = element(k), *old = b.CreateAlignedLoad(value->getType(), p, store->getAlign());
					b.CreateAlignedStore(b.CreateSelect(b.CreateICmpEQ(index, ConstantInt::get(index->getType(), k)), value, old), p, store->getAlign());
				}
			}
			i->eraseFromParent();
		}
		for (User *u : make_early_inc_range(g->users()))   // the parts' GEPs, used no more
			if (u->use_empty())
				cast<Instruction>(u)->eraseFromParent();
		if (g->use_empty())
			g->eraseFromParent();
	}
}

static std::string compile(Module &m, Function *entry, Function *air, bool pal, SmallVectorImpl<char> &object, bool *localArrays = nullptr) {
	// The AIR function and whatever it calls inlined into the entry; the module's other entry points go (they need not translate).
	for (const char *list : {"air.kernel", "air.vertex", "air.fragment"})
		for (unsigned i = 0; m.getNamedMetadata(list) && i < m.getNamedMetadata(list)->getNumOperands(); i++)
			if (auto *k = mdconst::dyn_extract_or_null<Function>(m.getNamedMetadata(list)->getOperand(i)->getOperand(0)); k && k != air) {
				k->replaceAllUsesWith(PoisonValue::get(k->getType()));
				k->eraseFromParent();
			}
	// Threadgroup variables have no initial value in Metal, but AIR can carry one (vkQuake's SPIRV-Cross light_mask: zeros), which
	// AMDGPU refuses for LDS: dropped.
	for (GlobalVariable &g : m.globals())
		if (g.getAddressSpace() == 3 && g.hasInitializer() && !isa<UndefValue>(g.getInitializer()))
			g.setInitializer(UndefValue::get(g.getValueType()));
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
	if (std::string why = linkRayQueries(m); !why.empty())
		return why;
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

	// A local struct reached only through GEPs of a constant field, as an alloca for each field: one field indexed at run time keeps SROA
	// off the whole struct (Cubase's dashed lines: a copy of {float4, ..., float[12], float, [12 x i8] padding}, its float[12] indexed),
	// and the field alone AMDGPU keeps in registers.
	SmallVector<AllocaInst *, 8> structs;   // (collected first: a split erases the instruction after the alloca too)
	for (Function &f : m)
		for (Instruction &i : instructions(f))
			if (auto *a = dyn_cast<AllocaInst>(&i); a && a->isStaticAlloca() && a->getAllocatedType()->isStructTy())
				structs.push_back(a);
	const DataLayout &dl = m.getDataLayout();
	for (AllocaInst *a : structs) {
		auto *st = cast<StructType>(a->getAllocatedType());
		if (st->isPacked() || a->use_empty())
			continue;
		// (reached through the flat cast legalize gave it, or directly)
		auto *flat = a->hasOneUse() ? dyn_cast<AddrSpaceCastInst>(*a->user_begin()) : nullptr;
		Value *base = flat ? static_cast<Value *>(flat) : a;
		const uint64_t first = st->getNumElements() ? dl.getTypeStoreSize(st->getElementType(0)) : 0;
		auto fieldAccess = [&](User *u) {
			// a load or store through the struct's own pointer: its first field (whose GEP folds away)
			if (auto *l = dyn_cast<LoadInst>(u))
				return l->isSimple() && dl.getTypeStoreSize(l->getType()) <= first;
			if (auto *w = dyn_cast<StoreInst>(u))
				return w->isSimple() && w->getPointerOperand() == base && dl.getTypeStoreSize(w->getValueOperand()->getType()) <= first;
			auto *g = dyn_cast<GetElementPtrInst>(u);
			return g && g->getPointerOperand() == base && g->getSourceElementType() == st && g->getNumIndices() >= 2 &&
			       match(g->getOperand(1), m_Zero()) && isa<ConstantInt>(g->getOperand(2));
		};
		if (base->use_empty() || !all_of(base->users(), fieldAccess))
			continue;
		IRBuilder<> fb(a);
		SmallVector<Value *, 16> fields;   // each field's alloca, as base's pointer type
		for (Type *t : st->elements()) {
			Value *field = fb.CreateAlloca(t, a->getAddressSpace());
			fields.push_back(flat ? fb.CreateAddrSpaceCast(field, flat->getType()) : field);
		}
		for (User *u : make_early_inc_range(base->users())) {
			auto *g = dyn_cast<GetElementPtrInst>(u);
			if (!g) {   // a load or store of the first field
				cast<Instruction>(u)->replaceUsesOfWith(base, fields[0]);
				continue;
			}
			const uint64_t n = cast<ConstantInt>(g->getOperand(2))->getZExtValue();
			Value *to = fields[n];
			if (g->getNumIndices() > 2) {
				SmallVector<Value *, 4> rest = {ConstantInt::get(g->getOperand(1)->getType(), 0)};
				rest.append(g->idx_begin() + 2, g->idx_end());
				IRBuilder<> gb(g);
				to = gb.CreateGEP(st->getElementType(n), fields[n], rest, "", g->isInBounds());
			}
			g->replaceAllUsesWith(to);
			g->eraseFromParent();
		}
		if (flat)
			flat->eraseFromParent();
		a->eraseFromParent();
	}

	// A local struct of 32-bit fields and arrays of them (Quake II's shadow lights: {float key[4]; uint idx[4]}) as an array of i32s:
	// AMDGPU's promotion takes arrays only, and a struct written at an index known only at run time stays in scratch otherwise. The
	// accesses are byte offsets on an opaque pointer, so only the alloca's type changes.
	std::function<bool(Type *)> words = [&](Type *t) {
		if (t->isFloatTy() || t->isIntegerTy(32))
			return true;
		if (auto *a = dyn_cast<ArrayType>(t))
			return words(a->getElementType());
		auto *s = dyn_cast<StructType>(t);
		return s && !s->isPacked() && all_of(s->elements(), words);
	};
	for (Function &f : m)
		for (Instruction &i : make_early_inc_range(instructions(f)))
			if (auto *a = dyn_cast<AllocaInst>(&i); a && a->getAllocatedType()->isStructTy() && a->isStaticAlloca() &&
			                                        words(a->getAllocatedType()))
				a->setAllocatedType(ArrayType::get(Type::getInt32Ty(m.getContext()), m.getDataLayout().getTypeAllocSize(a->getAllocatedType()) / 4));

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

	// A local array read at an index known only at run time (a shader's table of colours) stays an alloca, reached through flat pointers
	// (legalize's casts) that keep AMDGPU from promoting it into registers: for a shader, the pointers are made private again and the
	// backend promotes it. One whose array still needs scratch (which draws do not have) is refused after code generation by its stages'
	// .scratch_memory_size (*localArrays). Only shaders with such an array: run on all of them, the passes gave one of WindowServer's 16
	// bytes of scratch, and the size is set without any scratch used in 29 of WindowServer's that run (2026-11-17).
	const bool allocas = any_of(instructions(*entry), [](const Instruction &i) { return isa<AllocaInst>(i); });

	if (localArrays)
		*localArrays = allocas;
	if (allocas) {
		FunctionPassManager infer, split;
		infer.addPass(InferAddressSpacesPass(0));
		infer.run(*entry, fam);
		constantIndices(*entry);
		split.addPass(SROAPass(SROAOptions::ModifyCFG));
		split.addPass(InstCombinePass());
		split.run(*entry, fam);
	}

	raw_svector_ostream objectOut(object);
	legacy::PassManager cg;
	if (tm->addPassesToEmitFile(cg, objectOut, nullptr, CodeGenFileType::ObjectFile))
		return "this LLVM cannot emit an AMDGPU object";
	cg.run(m);
	// A kernel's local array that still needs scratch: refused (the runtime's dispatches have none), by its descriptor's
	// private_segment_fixed_size (<kernel>.kd + 4).
	if (allocas && !pal) {
		auto obj = object::ObjectFile::createObjectFile(MemoryBufferRef(StringRef(object.data(), object.size()), "kernel"));
		if (!obj) {
			consumeError(obj.takeError());
			return "the kernel object cannot be read";
		}
		for (const object::SymbolRef &sym : (*obj)->symbols()) {
			auto name = sym.getName();
			auto sec = sym.getSection();
			if (!name || *name != (entry->getName() + ".kd").str() || !sec || *sec == (*obj)->section_end()) {
				if (!name) consumeError(name.takeError());
				if (!sec) consumeError(sec.takeError());
				continue;
			}
			auto bytes = (*sec)->getContents();
			auto at = sym.getValue();
			uint32_t scratch = 0;
			if (!bytes || !at || *at + 8 > bytes->size()) {
				if (!bytes) consumeError(bytes.takeError());
				if (!at) consumeError(at.takeError());
				return "the kernel's descriptor cannot be read";
			}
			memcpy(&scratch, bytes->data() + *at + 4, 4);
			if (scratch)
				return "the kernel needs " + std::to_string(scratch) + " bytes of scratch memory (a local array not kept in registers)";
		}
	}
	return "";
}

static bool gLldSpent;   // lld said it cannot be run again in this process: --serve exits after its reply (launchd starts a new one)

struct Kernel {
	std::string hsaco;   // the linked code object
	unsigned buffers;
	uint32_t written;
	uint32_t texturesWritten;   // bit i: texture index i is written (or read and written)
};

// A discard's path made to return, before the shader is inlined into its entry, so that the entry's one `done` export comes after every
// discard. Left `unreachable`, the export sat on the other branch: a wave whose lanes all died on this path had exported `done` already,
// and the discard's early exit exported `done` again, an illegal instruction that stops the wave for good and hung the gfx ring (OpenMW's
// alpha-tested draws, 2026-11-23).
static void returnAfterDiscards(Module &m) {
	for (Function &f : m)
		for (BasicBlock &bb : f) {
			auto *end = dyn_cast<UnreachableInst>(bb.getTerminator());
			const bool discards = end && any_of(bb, [](Instruction &i) {
				auto *c = dyn_cast<CallInst>(&i);
				Function *callee = c ? c->getCalledFunction() : nullptr;
				return callee && callee->getName().starts_with("air.") && callee->getName().ends_with("discard_fragment");
			});
			if (!discards)
				continue;
			Type *type = f.getReturnType();
			ReturnInst::Create(m.getContext(), type->isVoidTy() ? nullptr : PoisonValue::get(type), end->getIterator());
			end->eraseFromParent();
		}
}

// A vertex or fragment function as a gfx12 shader with the hardware ABI of the kext's hand-written triangle shaders, for the host's draws
// (src/pvgpu.cpp), as an RGFX blob: {char[4] "RGFX", u32 stage (1 vertex, 2 fragment), u32 code bytes, u32 SPI_SHADER_PGM_RSRC1, u32 RSRC2,
// u32 SPI_PS_INPUT_ENA, u32 SPI_PS_INPUT_ADDR, u32 attributes (bits 0-15 the values between the stages; bit 16 its textures' levels read
// (RDNA4_MTLC_LEVEL_FIRST); bit 17 it writes a buffer or a texture; bit 18 dual-source blending (the colour and [[color(0), index(1)]] to the dual-source targets); bit 19 the depth
// exported (MRTZ, 32_R); bits 20-27 the colour attachments
// written when several, chosen by the host; bits 28-30 the one attachment other than 0 written; bit 31: a fragment shader without a colour)}, then the code
// (relocation-free, the entry at its start), then per attribute
// {u32 hash of its AIR name (FNV-1a), u32 flags: bit 0 flat, bit 1 the point's coordinate (no name)}: the vertex function's outputs in ring-slot order, the fragment function's
// inputs in hardware-input order; the host matches them by name (SPI_PS_INPUT_CNTL_n). Then {"TXRD", u32 bit n: texture n is used}, and for a
// vertex shader whose position is a buffer's float4x4 times an attribute, {"POSM", u32 the attribute, u32 the buffer}.
// Both stages get two user SGPRs, the address of the stage's table (the host's): 64-bit buffer addresses by Metal buffer index at +0, and in
// both stages the textures' T#s (8 dwords) at +64 + 32 n and the samplers' S#s (4 dwords) at +576 + 16 n (n 0-15).
//   vertex   amdgpu_gs, an NGG shader as src/shaders/nggcol.s: s3 = merged_wave_info ([7:0] vertices, [15:8] primitives, [27:24] wave in
//            the subgroup), s5 = gs_attr_offset (s0-s7 are the merged stage's system SGPRs), s8-s9 the table, s10 the first vertex
//            (added to VertexID, as RADV's base vertex), s11 the base instance (added to InstanceID), v0 = the packed primitive, v3 = VertexID,
//            v4 = InstanceID (gfx12; the host sets ES_VGPR_COMP_CNT 1, as RADV); the position goes out as pos0, the primitive
//            as prim, the other outputs to the attribute ring, output n in slot n (buffer_store_b128 at index = thread in the subgroup,
//            offset 16 n, soffset = gs_attr_offset << 9, scope device; the ring's descriptor from the literals 0xc01a0001-3, which the
//            host patches, as nggcol.s and Mesa's ac_nir_store_parameters_to_attr_ring). So far: vertex_id, instance_id, buffers.
//   fragment amdgpu_ps exporting render target 0 as four floats (SPI_SHADER_COL_FORMAT 32_ABGR), s0-s1 the table, PrimMask in s2;
//            buffers, textures, samplers, the position, the point coordinate and inputs, input n from hardware input n: interpolated at the centre (centroid
//            too, single-sample targets only so far), with or without perspective (ds_param_load + v_interp_p10/p2, as pscol.s), or flat (ds_param_load and quad lane 0,
//            the provoking vertex's value, as ACO's emit_interp_mov_instr).
// A value between the stages is up to four 32-bit words: float, half, int, short, char or bool, scalar or vector (half widened to float,
// smaller integers zero-extended).
static std::string translateShader(Module &m, Function *air, const MDNode *md, bool vertex, Kernel &out) {
	if (!vertex)
		returnAfterDiscards(m);
	LLVMContext &ctx = m.getContext();
	Type *i32 = Type::getInt32Ty(ctx), *f32 = Type::getFloatTy(ctx);
	const auto *outputs = md->getNumOperands() > 1 ? dyn_cast<MDNode>(md->getOperand(1)) : nullptr;
	const auto *inputs = md->getNumOperands() > 2 ? dyn_cast<MDNode>(md->getOperand(2)) : nullptr;
	if (!outputs || !inputs)
		return "the AIR shader's metadata is not as expected";
	// The inputs: index -> where its value comes from (-1: unused, poison).
	// 0 vertex id, 1 instance id, 2 base instance, 3 the fragment's position, 4 colour attachment 0, 5 front facing, 100 + n buffer n, 200 + n texture n, 300 + n sampler n,
	// 400 + n input n
	std::map<unsigned, int> from;
	// A value between the stages (index: its argument or member); sprite: the point's coordinate, which the hardware makes (no name)
	struct Var { unsigned index; uint32_t hash; bool flat; unsigned bary; bool sprite = false; };
	std::vector<Var> vars;
	uint32_t written = 0;   // bit n: buffer n is device memory the shader may write (the host maps it for writing)
	bool writesTexture = false;
	uint32_t texturesRead = 0;   // bit n: texture n is an argument the shader uses (the blob's "TXRD" record)
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
		else if (vertex && kind == "air.vertex_input" && !unused) {   // [[stage_in]]: fetched as the pipeline's vertex descriptor says
			int location = -1;
			for (unsigned k = 2; k + 1 < a->getNumOperands(); k++)
				if (mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
			if (location < 0 || location >= static_cast<int>(RDNA4_MTLC_MAX_ATTRIBUTES))
				return "vertex input " + std::to_string(index) + ": only attributes 0-15 so far";
			from[index] = 500 + location;
		}
		else if (vertex && kind == "air.instance_id")
			from[index] = 1;
		else if (vertex && kind == "air.base_instance")   // ANGLE's vertex shaders (WebGL) read it
			from[index] = 2;
		else if (!vertex && kind == "air.fragment_input" && !unused) {
			// The interpolation: air.flat, or air.center / air.centroid with air.perspective / air.no_perspective; the hardware's
			// barycentric inputs PERSP_CENTER 4 and LINEAR_CENTER 8 (the arguments below).
			bool flat = false, centroid = false, linear = false, center = false;   // centroid: accepted, read at the centre (below)
			for (unsigned k = 3; k < a->getNumOperands(); k++) {
				const std::string q = mdString(a, k);
				flat |= q == "air.flat";
				centroid |= q == "air.centroid";
				// per sample (air.sample: Cubase's line caps) read at the centre as well: no per-sample shading, one sample a pixel
				center |= q == "air.center" || q == "air.sample";
				linear |= q == "air.no_perspective";
			}
			if (!flat && !center && !centroid)
				return "fragment input " + std::to_string(index) + ": only center, centroid or flat interpolation so far";
			// ponytail: centroid read at the centre, exact for the single-sample targets drawn so far (the card's PERSP_CENTROID is
			// wrong next to PERSP_CENTER, rig 2026-10-22, each alone right; radeonsi also forces the centre without MSAA); MSAA needs it back.
			from[index] = 400 + static_cast<int>(vars.size());
			vars.push_back({index, fnv(mdString(a, 2)), flat, linear ? 8u : 4u});
		}
		else if (!vertex && kind == "air.point_coord" && !unused) {
			// [[point_coord]]: an input the SPI fills with the point sprite's S, T (PT_SPRITE_TEX); undefined for lines and triangles
			from[index] = 400 + static_cast<int>(vars.size());
			vars.push_back({index, 0, false, 4u, true});
		}
		else if (!vertex && kind == "air.position" && !unused)
			from[index] = 3;
		else if (!vertex && kind == "air.front_facing" && !unused)
			from[index] = 5;   // FRONT_FACE: > 0 for a front-facing primitive (Mesa's ac_nir_lower_intrinsics_to_args)
		// ponytail: no per-sample shading, [[sample_id]] is 0: exact on targets of one sample, on MSAA ones each pixel reads its first
		// sample (vkQuake's transparency resolve); PS_ITER_SAMPLES and the ancillary VGPR's sample when that shows
		else if (!vertex && kind == "air.sample_id" && !unused)
			from[index] = 6;
		else if (!vertex && kind == "air.render_target" && !unused && mdInt(a, 2) == 0)
			from[index] = 4;   // colour attachment 0 as it is (programmable blending): read at the pixel from the host's staged target
		// An argument buffer (air.indirect_buffer; MoltenVK's descriptor sets) is a buffer of 8-byte handles: a buffer's address, a texture's or
		// sampler's gpuResourceID = the address of its T# or S# (RDNA4MTLDevice), which the shader loads and passes on as handles.
		// An acceleration structure (ray queries in a fragment shader: MetalQuake's shadow rays) by its address, as a buffer.
		else if ((kind == "air.buffer" || kind == "air.indirect_buffer" || kind == "air.primitive_acceleration_structure" ||
		          kind == "air.instance_acceleration_structure") && !unused) {
			int location = -1;
			bool writes = false;
			for (unsigned k = 2; k < a->getNumOperands(); k++) {
				if (k + 1 < a->getNumOperands() && mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
				writes |= mdString(a, k) == "air.read_write" || mdString(a, k) == "air.write";
			}
			Type *argType = index < air->arg_size() ? air->getArg(index)->getType() : nullptr;
			if (location < 0 || location >= static_cast<int>(RDNA4_MTLC_MAX_BUFFERS) || !argType ||
			    (argType != PointerType::get(ctx, 1) && argType != PointerType::get(ctx, 2))) {
				// its address space in the message (99: not a pointer, ?: no such argument)
				const std::string space = !argType ? "?" : std::to_string(argType->isPointerTy() ? argType->getPointerAddressSpace() : 99u);
				return "buffer argument " + std::to_string(index) + " (index " + std::to_string(location) + ", " + space +
				       "): only device and constant buffers at indices 0-30 so far";
			}
			from[index] = 100 + location;
			written |= writes ? 1u << location : 0;
		} else if ((kind == "air.texture" || kind == "air.sampler") && !unused) {
			int location = -1, count = 1;   // count: of an array (array<texture2d, n>), its n
			for (unsigned k = 2; k + 1 < a->getNumOperands(); k++)
				if (mdString(a, k) == "air.location_index") {
					location = static_cast<int>(mdInt(a, k + 1));
					count = k + 2 < a->getNumOperands() ? static_cast<int>(mdInt(a, k + 2)) : 1;
				}
			const bool texture = kind == "air.texture";   // textures 0-30 (an array within 0-15 or 16-30: include/rdna4mtlc.h), samplers 0-15
			// past 30, or an array across those ranges: through the stage's texture handles (the native device's; textures 0-127)
			const bool handles = texture && (location + count > static_cast<int>(RDNA4_MTLC_TEXTURES) || (location < 16 && location + count > 16));
			if (location < 0 || count < 1 || location + count > (!texture ? 16 : handles ? static_cast<int>(RDNA4_MTLC_MAX_TEXTURES)
			                                                                             : static_cast<int>(RDNA4_MTLC_TEXTURES)) ||
			    (!texture && location < 16 && location + count > 16))
				return kind + " argument " + std::to_string(index) + ": only indices 0-" + (texture ? "127" : "15") + " so far";
			for (unsigned k = 2; texture && k < a->getNumOperands(); k++)
				writesTexture |= mdString(a, k) == "air.read_write" || mdString(a, k) == "air.write";
			from[index] = (handles ? 1000 : texture ? 200 : 300) + location;   // (1000: past the attributes' 500 + n)
			for (int n = location; texture && n < location + count && n < static_cast<int>(RDNA4_MTLC_TEXTURES); n++)
				texturesRead |= 1u << n;   // (the handles of 0-30 are their T#s' places in the table: those filled too)
			texturesRead |= handles ? RDNA4_MTLC_TXRD_HANDLES : 0;
		}
		else if (unused)
			from[index] = -1;
		else
			return std::string(vertex ? "vertex" : "fragment") + " input " + kind + " is not supported yet";
	}
	if (from.size() != air->arg_size())
		return "not every argument of the AIR shader is described";
	// A fragment function's colour: float or half, scalar or up to four, alone or as the struct member that is colour attachment 0, or the
	// one other attachment it writes (below). The host draws into one attachment: other attachments' values are dropped (CoreAnimation's mask
	// blit writes the same value to two).
	unsigned colourMember = 0;
	auto colourType = [&](Type *t) {
		if (auto *st = dyn_cast<StructType>(t))
			t = colourMember < st->getNumElements() ? st->getElementType(colourMember) : nullptr;
		auto *vt = t ? dyn_cast<FixedVectorType>(t) : nullptr;
		Type *e = vt ? vt->getElementType() : t;
		// or 32-bit integers (WindowServer's brim_init_lpf/brim_jump_lpf on Tahoe: int2 into an RG32 integer target): their bits exported
		// as they are, the host's SPI_SHADER_COL_FORMAT and CB format saying integer
		return e && (e == f32 || e->isHalfTy() || e == i32) && (!vt || vt->getNumElements() <= 4);
	};
	// (Read now: compile() below deletes the AIR function once it is inlined.)
	const bool noColour = !vertex && air->getReturnType()->isVoidTy();
	int position = -1, maskMember = -1;
	bool colourZero = false;
	// Its colour attachment: 0 when it writes that one (the others' values are dropped), else the one other it writes (CoreGraphics' path
	// coverage and a CoreAnimation mask write a half2 to attachment 1 only, Tahoe 26.6.2): the host draws into that attachment's texture.
	int attachment = -1, other = -1, otherIndex = 0;
	unsigned others = 0;
	int memberOf[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // the struct member of each colour attachment it writes
	// Dual-source blending: [[color(0), index(1)]], the blend's second source (Sublime Text's subpixel text through AppleMetalOpenGLRenderer
	// blends with Source1Color): exported beside the colour (header bit 18), not as it
	int dualMember = -1;
	int depthMember = -1;   // the struct member that is the fragment's depth
	for (unsigned k = 0; k < outputs->getNumOperands(); k++) {
		const auto *o = cast<MDNode>(outputs->getOperand(k));
		const std::string kind = mdString(o, 0);
		if (vertex && kind == "air.position")
			position = static_cast<int>(k);
		else if (vertex && kind == "air.vertex_output")
			vars.push_back({k, fnv(mdString(o, 1)), false, 0});
		else if (vertex && kind == "air.point_size")
			continue;   // only triangles are drawn: a point's size is not used
		// ponytail: clip distances dropped: what a user clip plane would cut is drawn (OpenMW's water reflection draws what is under the
		// water too); the hardware's (POS1 export, PA_CL_VS_OUT_CNTL CLIP_DIST_ENA) when that shows
		else if (vertex && kind == "air.clip_distance")
			continue;
		// ponytail: the layer is dropped: right for targets of one layer (MoltenVK's vtxCmdBlitImageLayered into vkQuake's 2D images),
		// an array target's draws all land in its first layer; export it in the misc position (PA_CL_VS_OUT_CNTL) when one needs it
		else if (vertex && kind == "air.render_target_array_index")
			continue;
		// ponytail: [[sample_mask]] only as "none": a pixel whose mask is 0 is discarded, the others write every sample (exact for one
		// sample; vkQuake's ambient-occlusion pass on MSAA); the mask export (DB_SHADER_CONTROL.MASK_EXPORT_ENABLE, MRTZ) when that shows
		else if (!vertex && kind == "air.sample_mask")
			maskMember = static_cast<int>(k);
		else if (!vertex && kind == "air.render_target" && mdInt(o, 1) == 0 && o->getNumOperands() > 2 && mdInt(o, 2) == 1)
			dualMember = static_cast<int>(k);
		else if (!vertex && kind == "air.render_target" && mdInt(o, 1) == 0) {
			memberOf[0] = static_cast<int>(k);
			colourMember = k;
			colourZero = colourType(air->getReturnType());
			attachment = 0;
		} else if (!vertex && kind == "air.render_target" && mdInt(o, 1) > 0 && mdInt(o, 1) < 8) {
			others++;
			memberOf[mdInt(o, 1)] = static_cast<int>(k);
			other = static_cast<int>(k);
			otherIndex = static_cast<int>(mdInt(o, 1));
		} else if (!vertex && kind == "air.depth") {
			// [[depth(any)]] (vkQuake's gl_FragDepth through MoltenVK): exported to MRTZ before the colour, header bit 19 (the host sets
			// SPI_SHADER_Z_FORMAT 32_R and DB_SHADER_CONTROL's Z_EXPORT_ENABLE, late Z)
			depthMember = static_cast<int>(k);
			continue;
		} else if (!(!vertex && kind == "air.render_target")) {
			return std::string(vertex ? "vertex" : "fragment") + " output " + kind + " is not supported yet";
		}
	}
	// A position that is a float4x4 in a buffer times a [[stage_in]] attribute (Core Animation's mvp_matrix * position): {"POSM", attribute,
	// buffer} after the TXRD record, for the native device to bound the draw's pixels from its vertices (the rows a draw reading its own
	// target needs copied: Core Animation's menu-bar blends are quads of the bar's rows with a scissor of the whole 4K target, 2026-11-20).
	uint32_t posm[3] = {};
	if (vertex && position >= 0)
		for (BasicBlock &bb : *air)
			if (auto *ret = dyn_cast<ReturnInst>(bb.getTerminator()); ret && ret->getReturnValue()) {
				Value *pos = ret->getReturnValue();
				while (auto *iv = dyn_cast<InsertValueInst>(pos))   // the struct's position member
					pos = iv->getIndices()[0] == static_cast<unsigned>(position) ? iv->getInsertedValueOperand() : iv->getAggregateOperand();
				if (isa<StructType>(pos->getType()))
					continue;
				SmallVector<Value *, 8> work{pos}, terms;
				while (!work.empty() && terms.size() <= 4) {
					Value *v = work.pop_back_val();
					if (auto *add = dyn_cast<BinaryOperator>(v); add && add->getOpcode() == Instruction::FAdd)
						work.append({add->getOperand(0), add->getOperand(1)});
					else
						terms.push_back(v);
				}
				unsigned seen = 0;
				const Argument *buf = nullptr, *attr = nullptr;
				for (Value *t : terms) {
					auto *mul = dyn_cast<BinaryOperator>(t);
					if (!mul || mul->getOpcode() != Instruction::FMul)
						break;
					for (unsigned o = 0; o < 2; o++) {   // column c of the matrix times a splat of the attribute's component c, either way round
						auto *col = dyn_cast<LoadInst>(mul->getOperand(o));
						auto *splat = dyn_cast<ShuffleVectorInst>(mul->getOperand(1 - o));
						if (!col || !splat || !col->getType()->isVectorTy() || splat->getShuffleMask().size() != 4)
							continue;
						const int c = splat->getShuffleMask()[0];
						bool same = c >= 0 && c < 4;
						for (int e : splat->getShuffleMask())
							same &= e == c;
						APInt off(64, 0);
						const auto *base = dyn_cast<Argument>(col->getPointerOperand()->stripAndAccumulateConstantOffsets(m.getDataLayout(), off, true));
						const auto *a = dyn_cast<Argument>(splat->getOperand(0));
						if (!same || !base || !a || off != 16 * c || (buf && buf != base) || (attr && attr != a))
							continue;
						buf = base, attr = a, seen |= 1u << c;
						break;
					}
				}
				const int b = buf ? from[buf->getArgNo()] : -1, v = attr ? from[attr->getArgNo()] : -1;
				if (terms.size() == 4 && seen == 15 && b >= 100 && b < 100 + static_cast<int>(RDNA4_MTLC_MAX_BUFFERS) && v >= 500 && v < 516)
					posm[0] = 0x4d534f50 /* "POSM" */, posm[1] = static_cast<uint32_t>(v - 500), posm[2] = static_cast<uint32_t>(b - 100);
				break;
			}
	if (!vertex && !noColour && attachment < 0 && others == 1) {
		colourMember = static_cast<unsigned>(other);
		colourZero = colourType(air->getReturnType());
		attachment = otherIndex;
	}
	// Several attachments of colour types (RenderBox's paint shaders on Tahoe write a colour and a coverage; the pipeline may mask either
	// off): each exported as the host says (RDNA4_MTLC_TABLE_ATTACHMENT), into that attachment's texture; header bits 20-27.
	uint32_t writes = 0;
	if (!vertex && !noColour && air->getReturnType()->isStructTy()) {
		const unsigned chosen = colourMember;
		for (unsigned n = 0; n < 8; n++) {
			colourMember = memberOf[n] < 0 ? ~0u : static_cast<unsigned>(memberOf[n]);
			writes |= memberOf[n] >= 0 && colourType(air->getReturnType()) ? 1u << n : 0;
		}
		colourMember = chosen;
		writes = __builtin_popcount(writes) > 1 ? writes : 0;
	}
	if (!vertex && !noColour && !colourZero) {
		// what the function returns and each output's kind and index, for adding the case
		std::string what, type;
		raw_string_ostream ts(type);

		air->getReturnType()->print(ts);
		for (unsigned k = 0; k < outputs->getNumOperands(); k++) {
			const auto *o = cast<MDNode>(outputs->getOperand(k));

			what += " " + mdString(o, 0) + (o->getNumOperands() > 1 && isa<ConstantAsMetadata>(o->getOperand(1)) ? "(" + std::to_string(mdInt(o, 1)) + ")" : "");
		}

		return "fragment output: colour attachment 0 missing, or not up to four floats or halves; it returns " + ts.str() + ", outputs:" + what;
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
	// POS_Z, POS_W, FRONT_FACE); LLVM enables the ones used and reports them (SPI_PS_INPUT_ENA/ADDR).
	Type *v2f32 = FixedVectorType::get(f32, 2), *v3f32 = FixedVectorType::get(f32, 3), *v4f32 = FixedVectorType::get(f32, 4);
	SmallVector<Type *, 17> params = vertex ? SmallVector<Type *, 17>(17, i32)   // s0-s7, s8-s11, v0-v4
	                                        : SmallVector<Type *, 17>{i32, i32, i32, v2f32, v2f32, v2f32, v3f32, v2f32, v2f32, v2f32, f32, f32, f32, f32, f32, f32};
	// The arguments by name: vertex s3, s5, s8-s9, s10, s11, v0, v3; fragment s0-s1, s2, POS_X (the barycentrics: Var::bary).
	const unsigned kInfo = 3, kAttrOffset = 5, kFirstVertex = 10, kBaseInstance = 11, kPrim = 12, kVertexId = 15, kInstanceId = 16, kPrimMask = 2,
	               kPosX = 11, kFrontFace = 15;
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
	// buffer n's place in the stage's table (28-30 past both tables: RDNA4_MTLC_HIGH_BUFFERS, from the vertex table at +0x400 of the state
	// page or the fragment table at +0x800)
	const uint64_t high = vertex ? RDNA4_MTLC_HIGH_BUFFERS - 0x400 : RDNA4_MTLC_HIGH_BUFFERS + 0x20 - 0x800;
	auto bufferSlot = [&](unsigned n) -> uint64_t { return n < 8 ? 8 * n : n < 28 ? RDNA4_MTLC_TABLE_MORE_BUFFERS + 8 * (n - 8) : high + 8 * (n - 28); };
	auto buffer = [&](unsigned n, Type *t) -> Value * {   // buffer n's address from the stage's table (constant memory, scalar loads)
		Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(bufferSlot(n))), PointerType::get(ctx, 4));
		Value *address = b.CreateIntToPtr(b.CreateAlignedLoad(b.getInt64Ty(), at, Align(8)), PointerType::get(ctx, 1));
		return t == address->getType() ? address : b.CreateAddrSpaceCast(address, t);   // a constant buffer: made global again by legalize()
	};
	// A texture or sampler handle: the address of its T# (table + 64 + 32 n) or S# (table + 576 + 16 n), which legalize() loads.
	auto handle = [&](uint64_t offset, Type *t) -> Value * { return b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(offset)), t); };
	auto fragPosition = [&]() {        // fragment: the pixel's window position x, y, depth z and w (POS_X..W_FLOAT)
		Value *r = PoisonValue::get(v4f32);
		for (unsigned c = 0; c < 4; c++)
			r = b.CreateInsertElement(r, f->getArg(kPosX + c), c);
		return r;
	};
	auto framebuffer = [&]() -> Value * {   // fragment: colour attachment 0 at this pixel (image.load from the T# at its place in the table)
		Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(RDNA4_MTLC_TABLE_FRAMEBUFFER)), PointerType::get(ctx, 4));
		Value *t = b.CreateAlignedLoad(FixedVectorType::get(i32, 8), at, Align(16));
		return b.CreateIntrinsic(Intrinsic::amdgcn_image_load_2d, {v4f32, i32, t->getType()},
		                         {b.getInt32(15), b.CreateFPToUI(f->getArg(kPosX), i32), b.CreateFPToUI(f->getArg(kPosX + 1), i32), t, b.getInt32(0),
		                          b.getInt32(0)});
	};
	// Vertex input `location` of vertex `vid` (instance: InstanceID + base instance) as the AIR argument's type t: its record in the table
	// (RDNA4_MTLC_TABLE_ATTRIBUTES) gives the buffer, offset, stride, step and the format as a kind and a component count. Each component is
	// read from the dword holding it (offsets and strides are multiples of 4); components past the format's are (0, 0, 0, 1).
	auto attribute = [&](unsigned location, Value *vid, Value *instance, Type *t) -> Value * {
		Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(RDNA4_MTLC_TABLE_ATTRIBUTES + 16 * location)), PointerType::get(ctx, 4));
		Value *rec = b.CreateAlignedLoad(FixedVectorType::get(i32, 4), at, Align(16));
		Value *offset = b.CreateExtractElement(rec, uint64_t(0)), *stride = b.CreateExtractElement(rec, 1), *info = b.CreateExtractElement(rec, 2);
		Value *rate = b.CreateExtractElement(rec, 3);
		Value *slot = b.CreateAnd(info, 31), *kind = b.CreateAnd(b.CreateLShr(info, 8), 15), *count = b.CreateAnd(b.CreateLShr(info, 12), 7);
		Value *bgra = b.CreateICmpNE(b.CreateAnd(info, 1u << 15), b.getInt32(0)), *step = b.CreateAnd(b.CreateLShr(info, 16), 3);
		// the buffer's address: indices 0-7 at +8 n, 8-27 at RDNA4_MTLC_TABLE_MORE_BUFFERS, 28-30 at RDNA4_MTLC_HIGH_BUFFERS (MoltenVK binds
		// vertex buffers from 30 down)
		Value *slotAt = b.CreateSelect(b.CreateICmpULT(slot, b.getInt32(8)), b.CreateShl(slot, 3),
		                               b.CreateSelect(b.CreateICmpULT(slot, b.getInt32(28)),
		                                              b.CreateAdd(b.getInt32(RDNA4_MTLC_TABLE_MORE_BUFFERS - 64), b.CreateShl(slot, 3)),
		                                              b.CreateAdd(b.getInt32(static_cast<uint32_t>(high) - 8 * 28), b.CreateShl(slot, 3))));
		Value *base = b.CreateAlignedLoad(b.getInt64Ty(), b.CreateIntToPtr(b.CreateAdd(table(), b.CreateZExt(slotAt, b.getInt64Ty())),
		                                                                  PointerType::get(ctx, 4)), Align(8));
		// the element: per vertex, per instance (step rate 0: always the first), or constant
		Value *perInstance = b.CreateSelect(b.CreateICmpEQ(rate, b.getInt32(0)), b.getInt32(0),
		                                    b.CreateUDiv(instance, b.CreateSelect(b.CreateICmpEQ(rate, b.getInt32(0)), b.getInt32(1), rate)));
		Value *element = b.CreateSelect(b.CreateICmpEQ(step, b.getInt32(1)), vid, b.CreateSelect(b.CreateICmpEQ(step, b.getInt32(2)), perInstance,
		                                                                                         b.getInt32(0)));
		Value *start = b.CreateAdd(base, b.CreateZExt(b.CreateAdd(b.CreateMul(element, stride), offset), b.getInt64Ty()));
		// An attribute without a record (not in the descriptor, or of a format not handled) or whose buffer is not bound reads the
		// table, which is always mapped, and gets the defaults: an unbound buffer's address is 0, and a load from there faults the GPU.
		Value *valid = b.CreateAnd(b.CreateICmpNE(count, b.getInt32(0)), b.CreateICmpNE(base, b.getInt64(0)));
		start = b.CreateSelect(valid, start, table());
		count = b.CreateSelect(valid, count, b.getInt32(0));
		Value *last = b.CreateSelect(valid, b.CreateSub(count, b.getInt32(1)), b.getInt32(0));
		// log2 of a component's bytes by kind, two bits each: float 2, half 1, the 8-bit ones 0, the 16-bit ones 1, the 32-bit integers 2
		Value *shift = b.CreateAnd(b.CreateLShr(b.getInt32(0xa55006), b.CreateShl(kind, 1)), 3);
		Value *fv[4], *iv[4];
		for (unsigned c = 0; c < 4; c++) {
			Value *cc = b.CreateSelect(b.CreateICmpULT(b.getInt32(c), count), b.getInt32(c), last);
			Value *byte = b.CreateShl(cc, shift);
			Value *ptr = b.CreateIntToPtr(b.CreateAdd(start, b.CreateZExt(b.CreateAnd(byte, ~3u), b.getInt64Ty())), PointerType::get(ctx, 1));
			Value *bits = b.CreateLShr(b.CreateAlignedLoad(i32, ptr, Align(4)), b.CreateShl(b.CreateAnd(byte, 3), 3));
			Value *u8 = b.CreateAnd(bits, 255), *u16 = b.CreateAnd(bits, 0xffff);
			Value *s8 = b.CreateAShr(b.CreateShl(bits, 24), 24), *s16 = b.CreateAShr(b.CreateShl(bits, 16), 16);
			auto fk = [&](unsigned k) { return b.CreateICmpEQ(kind, b.getInt32(k)); };
			Value *half = b.CreateFPExt(b.CreateBitCast(b.CreateTrunc(bits, b.getInt16Ty()), b.getHalfTy()), f32);
			Value *minusOne = ConstantFP::get(f32, -1.0);
			Value *f = b.CreateBitCast(bits, f32);
			f = b.CreateSelect(fk(1), half, f);
			f = b.CreateSelect(fk(2), b.CreateFMul(b.CreateUIToFP(u8, f32), ConstantFP::get(f32, 1.0 / 255)), f);
			f = b.CreateSelect(fk(3), b.CreateMaxNum(b.CreateFMul(b.CreateSIToFP(s8, f32), ConstantFP::get(f32, 1.0 / 127)), minusOne), f);
			f = b.CreateSelect(fk(4), b.CreateUIToFP(u8, f32), f);
			f = b.CreateSelect(fk(5), b.CreateSIToFP(s8, f32), f);
			f = b.CreateSelect(fk(6), b.CreateFMul(b.CreateUIToFP(u16, f32), ConstantFP::get(f32, 1.0 / 65535)), f);
			f = b.CreateSelect(fk(7), b.CreateMaxNum(b.CreateFMul(b.CreateSIToFP(s16, f32), ConstantFP::get(f32, 1.0 / 32767)), minusOne), f);
			f = b.CreateSelect(fk(8), b.CreateUIToFP(u16, f32), f);
			f = b.CreateSelect(fk(9), b.CreateSIToFP(s16, f32), f);
			f = b.CreateSelect(fk(10), b.CreateSIToFP(bits, f32), f);
			f = b.CreateSelect(fk(11), b.CreateUIToFP(bits, f32), f);
			Value *n = b.CreateFPToSI(f, i32);   // an integer input of a float format (not Metal's), then the integer formats as they are
			n = b.CreateSelect(b.CreateOr(fk(4), fk(8)), b.CreateSelect(fk(4), u8, u16), n);
			n = b.CreateSelect(b.CreateOr(fk(5), fk(9)), b.CreateSelect(fk(5), s8, s16), n);
			n = b.CreateSelect(b.CreateOr(fk(10), fk(11)), bits, n);
			Value *present = b.CreateICmpULT(b.getInt32(c), count);
			fv[c] = b.CreateSelect(present, f, ConstantFP::get(f32, c == 3 ? 1.0 : 0.0));
			iv[c] = b.CreateSelect(present, n, b.getInt32(c == 3 ? 1 : 0));
		}
		for (Value **v : {fv, iv}) {   // BGRA formats: the first and third components swapped
			Value *x = v[0];
			v[0] = b.CreateSelect(bgra, v[2], v[0]);
			v[2] = b.CreateSelect(bgra, x, v[2]);
		}
		auto *vt = dyn_cast<FixedVectorType>(t);
		Type *e = vt ? vt->getElementType() : t;
		Value *r = vt ? PoisonValue::get(t) : nullptr;
		for (unsigned k = 0; k < components(t) && k < 4; k++) {
			Value *x = e->isIntegerTy() ? b.CreateTrunc(iv[k], e) : e->isHalfTy() ? b.CreateFPTrunc(fv[k], e) : fv[k];
			r = vt ? b.CreateInsertElement(r, x, k) : x;
		}
		return r;
	};
	// A texture or sampler argument: its handle, the address of its T# or S# in the table. An array of them (array<texture2d, n>: AIR passes a
	// pointer to n handles, byval) as such an array, made here: MetalQuake's UI shader picks one of 16 by a vertex value, and with the first
	// T#'s address in its place it read the T#'s bytes as handles and faulted at 0, the ring with it (2026-11-17).
	auto resource = [&](unsigned k, uint64_t at, uint64_t stride) -> Value * {
		auto *array = air->getArg(k)->hasByValAttr() ? dyn_cast<ArrayType>(air->getParamByValType(k)) : nullptr;

		if (!array)
			return handle(at, air->getArg(k)->getType());

		IRBuilder<> entry(&f->getEntryBlock(), f->getEntryBlock().getFirstInsertionPt());
		AllocaInst *handles = entry.CreateAlloca(array);

		for (unsigned i = 0; i < array->getNumElements(); i++)
			b.CreateStore(handle(at + stride * i, array->getElementType()), b.CreateConstInBoundsGEP2_32(array, handles, 0, i));
		return handles;
	};
	// A texture or an array of them through the stage's texture handles (RDNA4_MTLC_TABLE_TEXTURE_HANDLES): one, its handle loaded from there;
	// an array, a pointer into them for the shader to index (not copied: byval dropped, an array of 128 would need scratch memory, which draws
	// have none of).
	auto throughHandles = [&](unsigned k, unsigned n) -> Value * {
		const uint64_t at = RDNA4_MTLC_TABLE_TEXTURE_HANDLES + 8 * n;
		if (air->getArg(k)->hasByValAttr()) {
			air->removeParamAttr(k, Attribute::ByVal);
			return b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(at)), air->getArg(k)->getType());
		}
		Value *slot = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(at)), PointerType::get(ctx, 4));
		return b.CreateIntToPtr(b.CreateAlignedLoad(b.getInt64Ty(), slot, Align(8)), air->getArg(k)->getType());
	};
	auto callAir = [&](Value *vid) {
		SmallVector<Value *, 4> args;
		for (unsigned k = 0; k < air->arg_size(); k++)
			args.push_back(from[k] >= 1000 ? throughHandles(k, static_cast<unsigned>(from[k] - 1000))
			               : from[k] >= 500 ? attribute(static_cast<unsigned>(from[k] - 500), vid, b.CreateAdd(f->getArg(kInstanceId), f->getArg(kBaseInstance)),
			                                          air->getArg(k)->getType())
			               : from[k] == 0 ? b.CreateZExtOrTrunc(vid, air->getArg(k)->getType())   // uint or ushort
			               : from[k] == 1 ? b.CreateZExtOrTrunc(b.CreateAdd(f->getArg(kInstanceId), f->getArg(kBaseInstance)), air->getArg(k)->getType())   // with the base instance
			               : from[k] == 2 ? b.CreateZExtOrTrunc(f->getArg(kBaseInstance), air->getArg(k)->getType())
			               : from[k] >= 400 ? fromVec4(inValues[static_cast<unsigned>(from[k] - 400)], air->getArg(k)->getType())
			               : from[k] == 3 ? fromVec4(fragPosition(), air->getArg(k)->getType())
			               : from[k] == 4 ? fromVec4(framebuffer(), air->getArg(k)->getType())
			               : from[k] == 5 ? b.CreateZExtOrTrunc(b.CreateFCmpOGT(f->getArg(kFrontFace), ConstantFP::get(f32, 0.0)), air->getArg(k)->getType())
			               : from[k] == 6 ? Constant::getNullValue(air->getArg(k)->getType())
			               : from[k] >= 300 ? resource(k, RDNA4_MTLC_TABLE_SAMPLERS + 16 * static_cast<unsigned>(from[k] - 300), 16)
			               : from[k] >= 200 ? resource(k, RDNA4_MTLC_TEXTURE_AT(static_cast<unsigned>(from[k] - 200)), 32)
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
				if (vars[n].flat) {   // quad lane 0 holds P0 (DPP quad_perm 0, 0, 0, 0), in whole-quad mode as ACO: lane 0 can be uncovered
					Value *mov = b.CreateIntrinsic(Intrinsic::amdgcn_update_dpp, {i32}, {PoisonValue::get(i32), b.CreateBitCast(p, i32), b.getInt32(0),
					                                                                     b.getInt32(15), b.getInt32(15), b.getFalse()});
					x = b.CreateBitCast(b.CreateIntrinsic(Intrinsic::amdgcn_wqm, {i32}, {mov}), f32);
				} else {
					Value *bary = f->getArg(vars[n].bary), *i = b.CreateExtractElement(bary, uint64_t(0)), *j = b.CreateExtractElement(bary, 1);
					x = b.CreateIntrinsic(Intrinsic::amdgcn_interp_inreg_p2, {}, {p, j, b.CreateIntrinsic(Intrinsic::amdgcn_interp_inreg_p10, {}, {p, i, p})});
					x = b.CreateIntrinsic(Intrinsic::amdgcn_wqm, {f32}, {x});   // in whole-quad mode too, as ACO
				}
				v = b.CreateInsertElement(v, x, c);
			}
			inValues.push_back(v);
		}
		Value *colour = callAir(nullptr);
		if (maskMember >= 0)
			b.CreateIntrinsic(Intrinsic::amdgcn_kill, {}, {b.CreateICmpNE(b.CreateExtractValue(colour, static_cast<unsigned>(maskMember)), b.getInt32(0))});
		if (colour->getType()->isVoidTy()) {   // no colour (it writes textures, say): an mrt0 export with no channels ends the shader (gfx11+
			Value *undef = PoisonValue::get(f32);   // has no null target; the host sets SPI_SHADER_COL_FORMAT to none)
			b.CreateIntrinsic(Intrinsic::amdgcn_exp, {f32}, {b.getInt32(0), b.getInt32(0), undef, undef, undef, undef, b.getTrue(), b.getTrue()});
		} else {
			Value *out = toVec4(colour->getType()->isStructTy() ? b.CreateExtractValue(colour, colourMember) : colour);
			if (depthMember >= 0 && colour->getType()->isStructTy() && static_cast<unsigned>(depthMember) != colourMember) {
				Value *undef = PoisonValue::get(f32);
				b.CreateIntrinsic(Intrinsic::amdgcn_exp, {f32}, {b.getInt32(8), b.getInt32(1), b.CreateExtractValue(colour, static_cast<unsigned>(depthMember)),
				                                                 undef, undef, undef, b.getFalse(), b.getTrue()});   // mrtz: the depth in x
			} else {
				depthMember = -1;
			}
			if (writes) {   // the attachment the host draws into, from the table
				Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(RDNA4_MTLC_TABLE_ATTACHMENT)), PointerType::get(ctx, 4));
				Value *sel = b.CreateAlignedLoad(i32, at, Align(4));
				for (unsigned n = 0; n < 8; n++)
					if ((writes >> n & 1) && static_cast<unsigned>(memberOf[n]) != colourMember)
						out = b.CreateSelect(b.CreateICmpEQ(sel, b.getInt32(n)), toVec4(b.CreateExtractValue(colour, static_cast<unsigned>(memberOf[n]))), out);
			}
			{   // alpha to coverage, one sample a pixel: kept when the table's flag is 0 or the alpha is at least 0.5
				Value *at = b.CreateIntToPtr(b.CreateAdd(table(), b.getInt64(RDNA4_MTLC_TABLE_ALPHA_TO_COVERAGE)), PointerType::get(ctx, 4));
				Value *on = b.CreateICmpNE(b.CreateAlignedLoad(i32, at, Align(4)), b.getInt32(0));
				Value *alpha = b.CreateExtractElement(out, 3);
				b.CreateIntrinsic(Intrinsic::amdgcn_kill, {}, {b.CreateOr(b.CreateNot(on), b.CreateFCmpOGE(alpha, ConstantFP::get(f32, 0.5)))});
			}
			if (dualMember >= 0 && colour->getType()->isStructTy()) {
				// Both sources to the dual-source targets (21, 22), swizzled as gfx11+ wants them: Mesa's ac_build_dual_src_blend_swizzle (the
				// first source's odd and even lanes swapped, the even lanes swapped between the sources, the first's lanes swapped back)
				Value *second = toVec4(b.CreateExtractValue(colour, static_cast<unsigned>(dualMember)));
				Value *lane = b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {b.getInt32(-1), b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {b.getInt32(-1), b.getInt32(0)})});
				Value *even = b.CreateICmpEQ(b.CreateAnd(lane, 1), b.getInt32(0));
				auto swapPairs = [&](Value *v) { return b.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp8, {i32}, {v, b.getInt32(0xde54c1)}); };
				Value *c0[4], *c1[4];
				for (unsigned k = 0; k < 4; k++) {
					Value *s0 = swapPairs(b.CreateBitCast(b.CreateExtractElement(out, k), i32)), *s1 = b.CreateBitCast(b.CreateExtractElement(second, k), i32);
					Value *t0 = s0;
					s0 = b.CreateSelect(even, s1, s0);
					s1 = b.CreateSelect(even, t0, s1);
					c0[k] = b.CreateBitCast(swapPairs(s0), f32);
					c1[k] = b.CreateBitCast(s1, f32);
				}
				b.CreateIntrinsic(Intrinsic::amdgcn_exp, {f32}, {b.getInt32(21), b.getInt32(15), c0[0], c0[1], c0[2], c0[3], b.getFalse(), b.getTrue()});
				b.CreateIntrinsic(Intrinsic::amdgcn_exp, {f32}, {b.getInt32(22), b.getInt32(15), c1[0], c1[1], c1[2], c1[3], b.getTrue(), b.getTrue()});
			} else {
				exportF32(0, out, true);   // mrt0: as four floats (half widened; missing components 0, alpha 1)
			}
		}
	}
	b.CreateRetVoid();

	SmallVector<char, 0> object;
	bool localArrays = false;
	if (std::string why = compile(m, f, air, true, object, &localArrays); !why.empty())
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
	uint64_t scratch = 0;    // the bytes of scratch its stages reserve
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
				for (auto &stage : pipes.getArray()[0].getMap(true)[".hardware_stages"].getMap(true))
					if (stage.second.isMap())
						if (auto &size = stage.second.getMap(true)[".scratch_memory_size"]; size.getKind() == msgpack::Type::UInt)
							scratch = std::max<uint64_t>(scratch, size.getUInt());
				for (auto &kv : pipes.getArray()[0].getMap(true)[".registers"].getMap(true))
					for (unsigned k = 0; k < 4; k++)
						if (kv.first.getKind() == msgpack::Type::UInt && kv.first.getUInt() == want[k])
								regs[k] = static_cast<uint32_t>(kv.second.getUInt());
			}
			consumeError(std::move(err));
		}
	if (code.empty() || !regs[0])
		return "no code or no register values in the shader object";
	if (localArrays && scratch)
		return "the shader needs " + std::to_string(scratch) + " bytes of scratch memory (a local array not kept in registers)";
	if (f->hasFnAttribute("rdna4-levels") && texturesRead >> 16)
		return "explicit LODs with textures 16-30: their levels have no T#s yet";
	const uint32_t head[8] = {0x58464752 /* "RGFX" */, vertex ? 1u : 2u, static_cast<uint32_t>(code.size()), regs[0], regs[1], regs[2], regs[3],
	                          static_cast<uint32_t>(vars.size()) | writes << 20 | (attachment > 0 ? static_cast<uint32_t>(attachment) << 28 : 0) |
	                          (noColour ? 1u << 31 : 0) | (f->hasFnAttribute("rdna4-levels") ? 1u << 16 : 0) | (dualMember >= 0 ? 1u << 18 : 0) | (depthMember >= 0 ? 1u << 19 : 0) |
	                          (written || writesTexture ? 1u << 17 : 0)};
	out.hsaco.assign(reinterpret_cast<const char *>(head), sizeof(head));
	out.hsaco += code.str();
	for (const Var &v : vars) {
		const uint32_t entry[2] = {v.hash, (v.flat ? 1u : 0u) | (v.sprite ? 2u : 0u)};
		out.hsaco.append(reinterpret_cast<const char *>(entry), sizeof(entry));
	}
	// after the values: {"TXRD", u32 the textures it uses}: the native device copies or waits for those only (a texture left bound from an
	// earlier draw, Core Animation's own target, cost a copy of it a draw, 2026-11-20); the PV host reads up to the values only
	const uint32_t reads[2] = {0x44525854 /* "TXRD" */, texturesRead};
	out.hsaco.append(reinterpret_cast<const char *>(reads), sizeof(reads));
	if (posm[0])
		out.hsaco.append(reinterpret_cast<const char *>(posm), sizeof(posm));
	out.buffers = 0;
	out.written = written;
	out.texturesWritten = 0;
	return "";
}

// A render pipeline's linked functions (tools/metal/native/RDNA4MTLDevice.mm, its MTLLinkedFunctions): "RDLK", u32 count, then per entry
// {u32 name bytes, the name, u32 data bytes, AIR bitcode or a metallib}: the function to translate first, then the functions it calls through
// `X.MTL_VISIBLE_FN_REF` (RenderBox's custom shaders call their private `custom_fn` so: Tahoe's icons). Empty when malformed.
struct LinkEntry { StringRef name, data; };

static std::vector<LinkEntry> linkEntries(StringRef in) {
	std::vector<LinkEntry> out;
	const uint64_t n = in.size() >= 8 ? le(in, 4, 4) : 0;
	size_t at = 8;
	for (uint64_t i = 0; i < n; i++) {
		LinkEntry e;
		for (StringRef *part : {&e.name, &e.data}) {
			const uint64_t bytes = at + 4 <= in.size() ? le(in, at, 4) : ~0ull;
			if (bytes > in.size() - at - 4)
				return {};
			*part = in.substr(at + 4, bytes);
			at += 4 + bytes;
		}
		out.push_back(e);
	}
	return out;
}

// The linked functions of `linked` (after the first, the translated one) linked into `m`, and each call through `X.MTL_VISIBLE_FN_REF` made
// a call of X and inlined: the module as plain AIR again, which is what the rest of the translation knows.
static std::string linkFunctions(Module &m, const std::vector<LinkEntry> &linked) {
	for (size_t i = 1; i < linked.size(); i++) {
		std::string fn = linked[i].name.str();
		const StringRef bitcode = linked[i].data.starts_with("MTLB") ? functionBitcode(linked[i].data, linked[i].name, fn) : linked[i].data;
		if (bitcode.empty())
			return "no linked function " + fn;
		SMDiagnostic diag;
		std::unique_ptr<Module> other = parseIR(MemoryBufferRef(bitcode, "linked"), diag, m.getContext());
		if (!other)
			return "cannot read the linked function " + fn + ": " + diag.getMessage().str();
		other->setTargetTriple(m.getTargetTriple());
		other->setDataLayout(m.getDataLayout());
		if (Linker::linkModules(m, std::move(other)))
			return "the linked function " + fn + " does not link";
		Function *ref = m.getFunction(fn + ".MTL_VISIBLE_FN_REF"), *def = m.getFunction(fn);
		if (!ref || !def || def->isDeclaration())
			continue;
		SmallVector<CallInst *, 4> calls;
		for (User *u : ref->users())
			if (auto *c = dyn_cast<CallInst>(u))
				calls.push_back(c);
		for (CallInst *c : calls) {
			if (c->getFunctionType() != def->getFunctionType())
				return "a call of the linked function " + fn + " with another signature";
			c->setCalledFunction(def);
			InlineFunctionInfo ifi;
			if (!InlineFunction(*c, ifi).isSuccess())
				return "the linked function " + fn + " cannot be inlined";
		}
		if (ref->use_empty())
			ref->eraseFromParent();
		if (def->use_empty())
			def->eraseFromParent();
	}
	return "";
}

// The kernel `fname` of `input` (AIR bitcode, or a metallib holding it, or a container of linked functions) as a code object; the reason
// when it cannot be translated.
static std::string translate(StringRef input, StringRef requested, Kernel &out) {
	std::vector<LinkEntry> linkedFns;
	if (input.starts_with("RDLK")) {
		linkedFns = linkEntries(input);
		if (linkedFns.empty())
			return "a malformed container of linked functions";
		input = linkedFns[0].data;
	}
	std::string found = requested.str();
	const StringRef bitcode = input.starts_with("MTLB") ? functionBitcode(input, requested, found) : input;
	const StringRef fname = found;
	if (bitcode.empty())
		return "no function " + requested.str() + " in the metallib";
	LLVMContext ctx;
	SMDiagnostic diag;
	std::unique_ptr<Module> m = parseIR(MemoryBufferRef(bitcode, "air"), diag, ctx);
	if (!m)
		return "cannot read the AIR: " + diag.getMessage().str();
	if (const std::string why = linkFunctions(*m, linkedFns); !why.empty())
		return why;
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
	enum Role { Buffer, PosInGrid, PosInGroup, GroupInGrid, GroupSize, GridSize, IndexInGroup, Lane, SimdInGroup, SimdWidth, SimdsInGroup, Texture, Sampler, GroupsInGrid,
	            GroupBuffer };
	struct Arg { Role role; int location; };
	std::map<unsigned, Arg> roles;
	int maxLocation = -1;
	uint32_t written = 0, texturesWritten = 0, groupBuffers = 0;
	for (const MDOperand &op : args->operands()) {
		const auto *a = cast<MDNode>(op);
		const unsigned index = static_cast<unsigned>(mdInt(a, 0));
		const std::string kind = mdString(a, 1);
		if (kind == "air.buffer" && index < air->arg_size() && air->getArg(index)->getType() == PointerType::get(ctx, 3)) {
			roles[index] = {GroupBuffer, -1};   // threadgroup memory the app sizes at dispatch (setThreadgroupMemoryLength): LDS below
			groupBuffers++;
		} else if (kind == "air.buffer" || kind == "air.indirect_buffer" || kind == "air.primitive_acceleration_structure" ||
		           kind == "air.instance_acceleration_structure") {   // the structures: their address, as a buffer's
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
		} else if (kind == "air.texture" || kind == "air.sampler") {
			int location = -1;
			bool writes = false;
			for (unsigned k = 2; k < a->getNumOperands(); k++) {
				if (k + 1 < a->getNumOperands() && mdString(a, k) == "air.location_index")
					location = static_cast<int>(mdInt(a, k + 1));
				writes |= mdString(a, k) == "air.read_write" || mdString(a, k) == "air.write";
			}
			if (location < 0 || location > 15)
				return "argument " + std::to_string(index) + ": " + kind + " only at indices 0-15 so far";
			roles[index] = {kind == "air.texture" ? Texture : Sampler, location};
			texturesWritten |= kind == "air.texture" && writes ? 1u << location : 0;
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
		} else if (kind == "air.threadgroups_per_grid") {
			roles[index] = {GroupsInGrid, -1};
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
	params.push_back(Type::getInt64Ty(ctx));   // the texture and sampler table
	const std::string name = air->getName().str();
	air->setName(name + ".air");
	air->setLinkage(GlobalValue::InternalLinkage);
	air->addFnAttr(Attribute::AlwaysInline);
	// The kernel's symbol: at most 63 bytes, what the runtime loads by (RDNA4_MAX_NAME, NUL included); the only one in its code object, so
	// a longer name is cut (Core Image's stitched kernels'; RDNA4MTLDevice.mm and the paravirtual host load by the same cut name)
	Function *kernel = Function::Create(FunctionType::get(Type::getVoidTy(ctx), params, false), GlobalValue::ExternalLinkage, name.substr(0, 63),
	                                    m.get());
	kernel->setCallingConv(CallingConv::AMDGPU_KERNEL);
	kernel->addFnAttr("amdgpu-flat-work-group-size", "1,1024");
	IRBuilder<> b(BasicBlock::Create(ctx, "entry", kernel));
	Value *group[3], *item[3], *size[3], *actual[3], *grid[3], *pos[3], *groups[3];
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
		groups[d] = b.CreateUDiv(b.CreateAdd(grid[d], b.CreateSub(size[d], b.getInt32(1))), size[d]);   // whole and partial groups
	}
	// The thread's index in its group, x fastest: the order the hardware packs a group into waves of 32, Metal's SIMD groups.
	Value *linear = b.CreateAdd(item[0], b.CreateMul(size[0], b.CreateAdd(item[1], b.CreateMul(size[1], item[2]))));
	Value *groupThreads = b.CreateMul(actual[0], b.CreateMul(actual[1], actual[2]));
	BasicBlock *run = BasicBlock::Create(ctx, "run", kernel), *done = BasicBlock::Create(ctx, "done", kernel);
	b.CreateCondBr(inside, run, done);
	b.SetInsertPoint(run);
	SmallVector<Value *, 16> callArgs;
	// Threadgroup buffers (WindowServer's glass luminance kernels on Tahoe: a float4 a thread, 16 KiB for 1024): each a block of LDS of the
	// kernel's own, 32 KiB shared out among them. ponytail: a fixed size, not the dispatch's length; a kernel asking for more overruns it,
	// and the length would have to reach the host's dispatch (Compute.setThreadgroupMemoryLength) as LDS beyond the code object's.
	const uint64_t groupBufferBytes = groupBuffers ? (32768 / groupBuffers) & ~15ull : 0;
	for (unsigned i = 0; i < air->arg_size(); i++) {
		const Arg &a = roles[i];
		if (a.role == GroupBuffer) {
			auto *type = ArrayType::get(b.getInt8Ty(), groupBufferBytes);
			auto *lds = new GlobalVariable(*m, type, false, GlobalValue::InternalLinkage, PoisonValue::get(type), "threadgroup_buffer", nullptr,
			                               GlobalValue::NotThreadLocal, 3);
			lds->setAlignment(Align(16));
			callArgs.push_back(lds);
			continue;
		}
		// A constant buffer (AIR's address space 2) is cast here and made global memory again by legalize().
		Value *table = kernel->getArg(nBuffers + 6);
		const unsigned slot = static_cast<unsigned>(a.location);   // a texture's or sampler's index (0-15)
		// An array of textures or samplers (array<texture2d, n>: AIR passes a pointer to n handles, byval) as such an array, made here as
		// translateShader's resource() does: with the first T#'s address in its place, vkQuake's lightmap kernel (three textures) read the
		// T#'s bytes as handles and the compute queue hung on the garbage descriptors, the desktop flashing until a reboot (2026-11-18).
		auto *array = (a.role == Texture || a.role == Sampler) && air->getArg(i)->hasByValAttr() ? dyn_cast<ArrayType>(air->getParamByValType(i)) : nullptr;
		if (array) {
			const uint64_t stride = a.role == Texture ? 32 : 16;
			const uint64_t at = (a.role == Texture ? RDNA4_MTLC_TABLE_TEXTURES : RDNA4_MTLC_TABLE_SAMPLERS) + stride * slot;
			if (slot + array->getNumElements() > 16)
				return "argument " + std::to_string(i) + ": an array of " + std::to_string(array->getNumElements()) + " past index 15";
			IRBuilder<> entry(&kernel->getEntryBlock(), kernel->getEntryBlock().getFirstInsertionPt());
			AllocaInst *handles = entry.CreateAlloca(array);
			for (unsigned e = 0; e < array->getNumElements(); e++)
				b.CreateStore(b.CreateIntToPtr(b.CreateAdd(table, b.getInt64(at + stride * e)), array->getElementType()),
				              b.CreateConstInBoundsGEP2_32(array, handles, 0, e));
			callArgs.push_back(handles);
			continue;
		}
		Value *v = a.role == Buffer      ? b.CreateAddrSpaceCast(kernel->getArg(static_cast<unsigned>(a.location)), air->getArg(i)->getType())
		         : a.role == Texture     ? b.CreateIntToPtr(b.CreateAdd(table, b.getInt64(RDNA4_MTLC_TABLE_TEXTURES + 32 * slot)), air->getArg(i)->getType())
		         : a.role == Sampler     ? b.CreateIntToPtr(b.CreateAdd(table, b.getInt64(RDNA4_MTLC_TABLE_SAMPLERS + 16 * slot)), air->getArg(i)->getType())
		         : a.role == PosInGrid   ? asType(b, air->getArg(i)->getType(), pos)
		         : a.role == PosInGroup  ? asType(b, air->getArg(i)->getType(), item)
		         : a.role == GroupInGrid ? asType(b, air->getArg(i)->getType(), group)
		         : a.role == GroupSize   ? asType(b, air->getArg(i)->getType(), actual)
		         : a.role == GridSize    ? asType(b, air->getArg(i)->getType(), grid)
		         : a.role == GroupsInGrid ? asType(b, air->getArg(i)->getType(), groups)
		         : a.role == IndexInGroup ? linear
		         : a.role == Lane         ? b.CreateAnd(linear, 31)
		         : a.role == SimdInGroup  ? b.CreateLShr(linear, 5)
		         : a.role == SimdWidth    ? b.getInt32(32)
		                                  : b.CreateLShr(b.CreateAdd(groupThreads, b.getInt32(31)), 5);
		if (v && v->getType() != air->getArg(i)->getType() && v->getType()->isIntegerTy() && air->getArg(i)->getType()->isIntegerTy(16))
			v = b.CreateTrunc(v, air->getArg(i)->getType());   // a ushort index
		if (v && v->getType() != air->getArg(i)->getType())
			v = nullptr;   // the index arguments are uint or ushort
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
	out.texturesWritten = texturesWritten;
	return "";
}

// A stream that could not be opened writes to fd -1 and aborts the helper in its destructor; nobody may not write a directory root made.
static void dumpFile(const std::string &path, StringRef bytes) {
	std::error_code ec;
	raw_fd_ostream out(path, ec, sys::fs::OF_None);

	if (ec)
		return;

	out << bytes;
	out.close();
	out.clear_error();
}

// With AIR2AMDGPU_DUMP set to a directory: each request's library and what became of it, <request>-<name>.metallib and .rgfx, for
// translating them again by hand (air2amdgpu <lib> <name> <out>) and disassembling them.
static void dump(uint64_t req, const char *name, StringRef lib, const std::string &out) {
	const char *dir = getenv("AIR2AMDGPU_DUMP");

	if (!dir)
		return;

	const std::string base = std::string(dir) + "/" + std::to_string(req) + "-" + name;

	dumpFile(base + ".metallib", lib);
	dumpFile(base + ".rgfx", out);
}

// A library a function of which could not be translated, kept beside the cache (failed/<hash>-<name>.metallib, at most 256 of them), to
// translate it again by hand (air2amdgpu <lib> <name> <out>) on another machine: an app's shaders cannot be made without the app.
static void keepFailed(StringRef lib, const char *name) {
	static unsigned kept;
	const std::string dir = "/var/db/rdna4-air2amdgpu/failed";

	if (kept >= 256)
		return;
	sys::fs::create_directories(dir);
	const std::string path = dir + "/" + Twine::utohexstr(xxh3_64bits(arrayRefFromStringRef(lib))).str() + "-" + name + ".metallib";
	if (!sys::fs::exists(path)) {
		dumpFile(path, lib);
		kept++;
	}
}

// The translations of earlier runs, by library, function and this build of the translator: WindowServer asks for its 70-odd shaders on every
// boot, and each held the desktop for up to seconds while it was translated again. A file is {u32 buffers, u32 written, u32 textures
// written, code object}.
static const char *const kCacheDir = "/var/db/rdna4-air2amdgpu";

static std::string cachePath(StringRef lib, const char *name) {
	static const char build[] = __DATE__ " " __TIME__;
	const uint64_t key = xxh3_64bits(arrayRefFromStringRef(lib)) ^ xxh3_64bits(arrayRefFromStringRef(StringRef(name))) * 31 ^
	                     xxh3_64bits(arrayRefFromStringRef(StringRef(build)));

	return std::string(kCacheDir) + "/" + Twine::utohexstr(key).str() + "-" + std::to_string(lib.size()) + ".rgfx";
}

static bool cached(StringRef lib, const char *name, Kernel &out) {
	auto file = MemoryBuffer::getFile(cachePath(lib, name));
	uint32_t head[3];

	if (!file || (*file)->getBufferSize() <= sizeof(head))
		return false;

	memcpy(head, (*file)->getBufferStart(), sizeof(head));
	out.buffers = head[0];
	out.written = head[1];
	out.texturesWritten = head[2];
	out.hsaco = (*file)->getBuffer().drop_front(sizeof(head)).str();

	return true;
}

static void keep(StringRef lib, const char *name, const Kernel &k) {
	const std::string path = cachePath(lib, name), tmp = path + ".tmp";
	const uint32_t head[3] = {k.buffers, k.written, k.texturesWritten};
	std::error_code ec;

	sys::fs::create_directories(kCacheDir);
	{
		raw_fd_ostream out(tmp, ec, sys::fs::OF_None);

		if (ec)
			return;

		out.write(reinterpret_cast<const char *>(head), sizeof(head));
		out << k.hsaco;
		out.close();

		if (out.has_error()) {   // a full disk: no cache entry, and no abort in the stream's destructor
			out.clear_error();
			sys::fs::remove(tmp);

			return;
		}
	}

	sys::fs::rename(tmp, path);   // whole files only: a reader never sees half of one
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
		sleep(1);   // the host holds WindowServer's first draws until the helper is there
	}
	errs() << "air2amdgpu: connected\n";

	// Root only opens the door (the kext lets no one else): the metallibs any app sends are parsed and compiled as nobody, who may write
	// the cache and nothing else of the system's.
	constexpr uid_t kNobody = static_cast<uid_t>(-2);
	sys::fs::create_directories(kCacheDir);
	if (chown(kCacheDir, kNobody, static_cast<gid_t>(-2)) || setgroups(0, nullptr) || setgid(static_cast<gid_t>(-2)) || setuid(kNobody) ||
	    !setuid(0))
		return fail("could not drop root after connecting");
	errs() << "air2amdgpu: running as nobody\n";

	std::vector<char> lib(RDNA4_MTLC_MAX_LIB);
	for (;;) {
		const uint64_t in[2] = {reinterpret_cast<uint64_t>(lib.data()), lib.size()};
		uint64_t req[2] = {};
		uint32_t nReq = 2;
		char name[RDNA4_MTLC_NAME] = {};
		size_t nameLen = sizeof(name);
		const kern_return_t kr = IOConnectCallMethod(conn, kMtlcNext, in, 2, nullptr, 0, req, &nReq, name, &nameLen);
		if (kr != KERN_SUCCESS)
			return fail("the kext ended the session (0x" + Twine::utohexstr(kr).str() + ")");
		name[sizeof(name) - 1] = 0;
		Kernel k {};
		const StringRef library(lib.data(), req[1]);
		const bool hit = cached(library, name, k);
		const std::string why = hit ? "" : translate(library, name, k);

		if (!hit && why.empty())
			keep(library, name, k);

		const std::string what = (hit ? "cached, " : "translated, ") + std::to_string(k.hsaco.size()) + " bytes";

		errs() << "air2amdgpu: request " << req[0] << ": " << name << ": " << (why.empty() ? what : why) << "\n";
		dump(req[0], name, StringRef(lib.data(), req[1]), k.hsaco);
		if (!why.empty())
			keepFailed(library, name);
		const uint64_t reply[6] = {req[0], reinterpret_cast<uint64_t>(k.hsaco.data()), why.empty() ? k.hsaco.size() : 0, k.buffers, k.written,
		                           k.texturesWritten};
		IOConnectCallScalarMethod(conn, kMtlcDeliver, reply, 6, nullptr, nullptr);
		if (gLldSpent)
			return fail("lld cannot run again in this process; exiting");
	}
}

int main(int argc, char **argv) {
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmPrinters();
	InitializeAllAsmParsers();   // the inline asm of translateShader()
	// A local array is never moved into LDS: that slices it by the work-group size, read from the dispatch packet, which the kext's
	// dispatches do not give (kernelFits refuses a kernel asking for it). It goes into registers, or scratch, instead (Quake II's shadows).
	const char *llvmArgs[] = {"air2amdgpu", "-disable-promote-alloca-to-lds"};
	cl::ParseCommandLineOptions(2, llvmArgs);
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
