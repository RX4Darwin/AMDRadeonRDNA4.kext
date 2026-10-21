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
static std::string legalize(Function &k) {
	LLVMContext &ctx = k.getContext();
	PointerType *global = PointerType::get(ctx, 1);
	SmallVector<Instruction *, 32> gone;
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
		if (op.starts_with("simd_")) {
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
	if (air->use_empty())
		air->eraseFromParent();   // inlined: its original body would only have to be legalised too
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
// u32 SPI_PS_INPUT_ENA, u32 SPI_PS_INPUT_ADDR, u32 0}, then the code (relocation-free, the entry at its start).
//   vertex   amdgpu_gs, an NGG shader as src/shaders/nggcol.s: s3 = merged_wave_info ([7:0] vertices, [15:8] primitives), v0 = the packed
//            primitive, v3 = VertexID; the position goes out as pos0, the primitive as prim. So far: vertex_id (and instance_id, 0: the
//            host draws one instance), the position as the only output.
//   fragment amdgpu_ps exporting render target 0 as four floats (SPI_SHADER_COL_FORMAT 32_ABGR); so far no inputs.
static std::string translateShader(Module &m, Function *air, const MDNode *md, bool vertex, Kernel &out) {
	LLVMContext &ctx = m.getContext();
	Type *i32 = Type::getInt32Ty(ctx), *f32 = Type::getFloatTy(ctx);
	const auto *outputs = md->getNumOperands() > 1 ? dyn_cast<MDNode>(md->getOperand(1)) : nullptr;
	const auto *inputs = md->getNumOperands() > 2 ? dyn_cast<MDNode>(md->getOperand(2)) : nullptr;
	if (!outputs || !inputs)
		return "the AIR shader's metadata is not as expected";
	// The inputs: index -> where its value comes from (-1: unused, poison).
	std::map<unsigned, int> from;   // 0 vertex id, 1 zero
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
		else if (unused)
			from[index] = -1;
		else
			return std::string(vertex ? "vertex" : "fragment") + " input " + kind + " is not supported yet";
	}
	if (from.size() != air->arg_size())
		return "not every argument of the AIR shader is described";
	int position = -1;
	for (unsigned k = 0; k < outputs->getNumOperands(); k++) {
		const std::string kind = mdString(cast<MDNode>(outputs->getOperand(k)), vertex ? 0 : 0);
		if (vertex && kind == "air.position")
			position = static_cast<int>(k);
		else if (!(!vertex && kind == "air.render_target" && outputs->getNumOperands() == 1 && air->getReturnType() == FixedVectorType::get(f32, 4)))
			return std::string(vertex ? "vertex" : "fragment") + " output " + kind + " is not supported yet";
	}
	if (vertex && (position < 0 || outputs->getNumOperands() != 1))
		return "the vertex function must have the position as its only output (so far)";

	// The shader.
	const unsigned nArgs = vertex ? 10 : 0;   // vertex: s0-s5, v0-v3
	Function *f = Function::Create(FunctionType::get(Type::getVoidTy(ctx), SmallVector<Type *, 10>(nArgs, i32), false), GlobalValue::ExternalLinkage,
	                               vertex ? "vs" : "ps", &m);
	f->setCallingConv(vertex ? CallingConv::AMDGPU_GS : CallingConv::AMDGPU_PS);
	for (unsigned k = 0; k < 6 && vertex; k++)
		f->addParamAttr(k, Attribute::InReg);
	IRBuilder<> b(BasicBlock::Create(ctx, "entry", f));
	auto callAir = [&](Value *vid) {
		SmallVector<Value *, 4> args;
		for (unsigned k = 0; k < air->arg_size(); k++)
			args.push_back(from[k] == 0 ? vid : from[k] == 1 ? static_cast<Value *>(b.getInt32(0)) : PoisonValue::get(air->getArg(k)->getType()));
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
		Value *info = f->getArg(3);
		BasicBlock *pos = BasicBlock::Create(ctx, "pos", f), *afterPos = BasicBlock::Create(ctx, "after_pos", f);
		BasicBlock *prim = BasicBlock::Create(ctx, "prim", f), *done = BasicBlock::Create(ctx, "done", f);
		b.CreateCondBr(b.CreateICmpULT(lane, b.CreateAnd(info, 255)), pos, afterPos);
		b.SetInsertPoint(pos);
		Value *r = callAir(f->getArg(9));
		exportF32(12, r->getType()->isStructTy() ? b.CreateExtractValue(r, static_cast<unsigned>(position)) : r, false);   // pos0
		b.CreateBr(afterPos);
		b.SetInsertPoint(afterPos);
		b.CreateCondBr(b.CreateICmpULT(lane, b.CreateAnd(b.CreateLShr(info, 8), 255)), prim, done);
		b.SetInsertPoint(prim);
		Value *undef = PoisonValue::get(i32);
		b.CreateIntrinsic(Intrinsic::amdgcn_exp, {i32}, {b.getInt32(20), b.getInt32(1), f->getArg(6), undef, undef, undef, b.getTrue(), b.getFalse()});
		b.CreateBr(done);
		b.SetInsertPoint(done);
	} else {
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
	const uint32_t head[8] = {0x58464752 /* "RGFX" */, vertex ? 1u : 2u, static_cast<uint32_t>(code.size()), regs[0], regs[1], regs[2], regs[3], 0};
	out.hsaco.assign(reinterpret_cast<const char *>(head), sizeof(head));
	out.hsaco += code.str();
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
