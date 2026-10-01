// air2amdgcn: lower Apple AIR (Metal IR, LLVM bitcode with air.* intrinsics and air.* metadata) to LLVM IR that upstream LLVM's AMDGPU back end
// compiles for gfx1201 (RDNA4, wave32). Feasibility spike (docs/m3-air-spike.md), written from the AIR files themselves and public write-ups; no Apple code.
//
//   air2amdgcn kernel   IN.bc|IN.ll --tg X,Y,Z -o OUT.ll [--entry NAME]      one compute kernel -> an amdgpu_kernel (ABI below)
//   air2amdgcn coverage [--codegen] IN.bc|IN.ll ...                           lower every air.* call of every function in place, keep the signatures,
//                                                                              print one JSON line per file (what lowered, what is missing, whether the
//                                                                              AMDGPU back end compiles the result when --codegen)
//
// Kernel ABI produced (the contract between this compiler and the runtime that launches the kernel; "docs/m3-air-spike.md" section 4):
//   * every !air.buffer argument -> one 8-byte kernarg pointer, in AIR order: AIR address space 1 (device) -> addrspace(1) (global),
//     AIR address space 2 (constant) -> addrspace(4) (constant), everywhere in the module; texture/sampler/argument-buffer/imageblock arguments are refused;
//   * then three hidden u32 kernargs: the grid size in THREADS (x, y, z) (dispatchThreads semantic; dispatchThreadgroups = groups * tg);
//   * threadgroup size is a compile-time constant (--tg), as in a Metal pipeline (maxTotalThreadsPerThreadgroup / dispatch size are known at
//     pipeline creation); the launch must use groups = ceil(grid / tg) per dimension; threads outside the grid return at once (Metal has no such
//     threads: non-uniform threadgroups), which is safe before barriers because the hardware counts waves, not lanes;
//   * thread_position_in_grid / threadgroup_position_in_grid / thread_position_in_threadgroup / thread_index_in_threadgroup /
//     threads_per_threadgroup / threadgroups_per_grid / threads_per_grid / thread_index_in_simdgroup / simdgroup_index_in_threadgroup are
//     computed in a prologue from workgroup id, workitem id and the hidden grid size; a simdgroup is one wave32;
//   * threadgroup memory = the module's addrspace(3) globals (static LDS), unchanged.
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <string>

using namespace llvm;

namespace {

constexpr unsigned kAirDevice = 1, kAirConstant = 2, kAirThreadgroup = 3;
constexpr unsigned kAmdGlobal = 1, kAmdLocal = 3, kAmdConstant = 4;

std::string family(StringRef name) {   // air.fast_fmax.f32 -> air.fast_fmax ; air.convert.f.f32.s.i32 -> air.convert
	static const std::regex typePart("^(v[0-9]+)?[fisu][0-9]+$|^p[0-9]+[a-z0-9]*$|^[fsu]$|^(fast|precise)$");
	SmallVector<StringRef, 8> parts;
	name.split(parts, '.');
	std::string out = parts[0].str();
	for (size_t i = 1; i < parts.size(); i++) {
		if (std::regex_match(parts[i].str(), typePart))
			break;
		out += "." + parts[i].str();
	}
	return out;
}

// Per-run bookkeeping of what was lowered and what has no rule.
struct Stats {
	std::map<std::string, unsigned> lowered;       // family -> call sites lowered
	std::map<std::string, unsigned> unsupported;   // full callee name -> call sites left alone
};

// ---- helpers --------------------------------------------------------------------------------------------------------------------------------

using ScalarFn = std::function<Value *(IRBuilder<> &, ArrayRef<Value *>)>;

Value *elementwise(IRBuilder<> &B, Type *RT, ArrayRef<Value *> args, const ScalarFn &fn) {
	auto *VT = dyn_cast<FixedVectorType>(RT);
	if (!VT)
		return fn(B, args);
	Value *res = PoisonValue::get(VT);
	for (unsigned i = 0; i < VT->getNumElements(); i++) {
		SmallVector<Value *, 4> e;
		for (Value *a : args)
			e.push_back(a->getType()->isVectorTy() ? B.CreateExtractElement(a, i) : a);
		res = B.CreateInsertElement(res, fn(B, e), i);
	}
	return res;
}

Value *splat(IRBuilder<> &B, Type *like, Constant *scalar) {
	if (auto *VT = dyn_cast<FixedVectorType>(like))
		return ConstantVector::getSplat(VT->getElementCount(), scalar);
	return scalar;
}

Value *fconst(IRBuilder<> &B, Type *ty, double v) {
	Type *st = ty->getScalarType();
	return splat(B, ty, ConstantFP::get(st, v));
}

// Value of the sign marker in names like air.min.s.i32 / air.convert.f.f32.u.i32: 's', 'u' or ' '.
char signOf(ArrayRef<StringRef> t) {
	for (size_t i = t.size(); i-- > 1;)
		if (t[i] == "s" || t[i] == "u")
			return t[i][0];
	return ' ';
}

FastMathFlags fastFlags() {
	FastMathFlags f;
	f.setFast();
	return f;
}

// ---- air.* call lowering ----------------------------------------------------------------------------------------------------------------------

// Returns the replacement value (or nullptr + *why when there is no rule). `isVoid` calls return a non-null sentinel (the call itself).
Value *lowerCall(IRBuilder<> &B, CallInst *CI, StringRef name, std::string *why) {
	LLVMContext &C = B.getContext();
	SmallVector<StringRef, 8> tok;
	name.split(tok, '.');
	const std::string fam = family(name);
	const bool fast = StringRef(fam).starts_with("air.fast_");
	const std::string op = fast ? fam.substr(9) : fam.substr(4);   // "fmax", "sqrt", "convert", ...
	Type *RT = CI->getType();
	SmallVector<Value *, 4> A(CI->args());
	auto un = [&](Intrinsic::ID id) { return B.CreateUnaryIntrinsic(id, A[0]); };
	auto bin = [&](Intrinsic::ID id) { return B.CreateBinaryIntrinsic(id, A[0], A[1]); };
	auto fm = [&](Value *v) {
		if (fast)
			if (auto *I = dyn_cast<Instruction>(v))
				if (isa<FPMathOperator>(I))
					I->setFastMathFlags(fastFlags());
		return v;
	};
	if (op == "fmax" || op == "max" && signOf(tok) == ' ')
		return bin(Intrinsic::maxnum);
	if (op == "fmin" || op == "min" && signOf(tok) == ' ')
		return bin(Intrinsic::minnum);
	if (op == "max")
		return bin(signOf(tok) == 's' ? Intrinsic::smax : Intrinsic::umax);
	if (op == "min")
		return bin(signOf(tok) == 's' ? Intrinsic::smin : Intrinsic::umin);
	if (op == "fabs")
		return un(Intrinsic::fabs);
	if (op == "abs")   // integer abs (result is the unsigned type)
		return B.CreateBinaryIntrinsic(Intrinsic::abs, A[0], B.getFalse());
	if (op == "sqrt")
		return fm(un(Intrinsic::sqrt));
	if (op == "rsqrt")   // 1/sqrt(x): v_rsq (1 ulp-class); the precise variant is identical here, see docs
		return elementwise(B, RT, A, [&](IRBuilder<> &b, ArrayRef<Value *> e) { return b.CreateUnaryIntrinsic(Intrinsic::amdgcn_rsq, e[0]); });
	if (op == "floor")
		return un(Intrinsic::floor);
	if (op == "ceil")
		return un(Intrinsic::ceil);
	if (op == "trunc")
		return un(Intrinsic::trunc);
	if (op == "round")
		return un(Intrinsic::round);
	if (op == "rint")
		return un(Intrinsic::roundeven);
	if (op == "fract") {   // x - floor(x), clamped just below 1 (Metal)
		Value *fl = B.CreateUnaryIntrinsic(Intrinsic::floor, A[0]);
		Value *d = B.CreateFSub(A[0], fl);
		return B.CreateBinaryIntrinsic(Intrinsic::minnum, d, fconst(B, RT, 0x1.fffffep-1));
	}
	if (op == "fma")
		return B.CreateIntrinsic(Intrinsic::fma, {RT}, {A[0], A[1], A[2]});
	if (op == "mix")   // x + (y - x) * a
		return B.CreateFAdd(A[0], B.CreateFMul(B.CreateFSub(A[1], A[0]), A[2]));
	if (op == "saturate")
		return B.CreateBinaryIntrinsic(Intrinsic::minnum, B.CreateBinaryIntrinsic(Intrinsic::maxnum, A[0], fconst(B, RT, 0.0)), fconst(B, RT, 1.0));
	if (op == "clamp") {
		if (RT->getScalarType()->isFloatingPointTy())
			return B.CreateBinaryIntrinsic(Intrinsic::minnum, B.CreateBinaryIntrinsic(Intrinsic::maxnum, A[0], A[1]), A[2]);
		const bool s = signOf(tok) == 's';
		return B.CreateBinaryIntrinsic(s ? Intrinsic::smin : Intrinsic::umin, B.CreateBinaryIntrinsic(s ? Intrinsic::smax : Intrinsic::umax, A[0], A[1]), A[2]);
	}
	if (op == "sign") {   // x > 0 ? 1 : x < 0 ? -1 : 0 (NaN -> 0)
		Value *z = fconst(B, RT, 0.0);
		Value *pos = B.CreateSelect(B.CreateFCmpOGT(A[0], z), fconst(B, RT, 1.0), z);
		return B.CreateSelect(B.CreateFCmpOLT(A[0], z), fconst(B, RT, -1.0), pos);
	}
	if (op == "dot") {   // sum of products, left to right
		auto *VT = dyn_cast<FixedVectorType>(A[0]->getType());
		if (!VT) {
			return B.CreateFMul(A[0], A[1]);
		}
		Value *acc = B.CreateFMul(B.CreateExtractElement(A[0], uint64_t(0)), B.CreateExtractElement(A[1], uint64_t(0)));
		for (unsigned i = 1; i < VT->getNumElements(); i++)
			acc = B.CreateIntrinsic(Intrinsic::fma, {RT}, {B.CreateExtractElement(A[0], i), B.CreateExtractElement(A[1], i), acc});
		return acc;
	}
	if (op == "pow" || op == "powr")
		return fm(bin(Intrinsic::pow));
	if (op == "exp")
		return fm(un(Intrinsic::exp));
	if (op == "exp2")
		return fm(un(Intrinsic::exp2));
	if (op == "log")
		return fm(un(Intrinsic::log));
	if (op == "log2")
		return fm(un(Intrinsic::log2));
	if (op == "log10")
		return fm(un(Intrinsic::log10));
	if (op == "sin")
		return fm(un(Intrinsic::sin));
	if (op == "cos")
		return fm(un(Intrinsic::cos));
	if (op == "all")
		return B.CreateAndReduce(A[0]);
	if (op == "any")
		return B.CreateOrReduce(A[0]);
	if (op == "convert") {   // air.convert.<dk>.<dt>.<sk>.<st>: kinds f (float), s (signed), u (unsigned; also bool)
		if (tok.size() < 6) {
			*why = "malformed convert name";
			return nullptr;
		}
		const char dk = tok[2][0], sk = tok[4][0];
		Type *ST = A[0]->getType();
		const bool dBool = RT->getScalarType()->isIntegerTy(1), sBool = ST->getScalarType()->isIntegerTy(1);
		if (dk == 'f' && sk == 'f')
			return B.CreateFPCast(A[0], RT);
		if (dk == 'f')
			return sk == 's' && !sBool ? B.CreateSIToFP(A[0], RT) : B.CreateUIToFP(A[0], RT);
		if (sk == 'f') {
			if (dBool)
				return B.CreateFCmpUNE(A[0], fconst(B, ST, 0.0));
			return B.CreateIntrinsic(dk == 's' ? Intrinsic::fptosi_sat : Intrinsic::fptoui_sat, {RT, ST}, {A[0]});   // Metal saturates
		}
		if (dBool)
			return B.CreateICmpNE(A[0], Constant::getNullValue(ST));
		return sk == 's' ? B.CreateSExtOrTrunc(A[0], RT) : B.CreateZExtOrTrunc(A[0], RT);
	}
	if (fam == "air.wg.barrier" || fam == "air.simdgroup.barrier") {
		// air.wg.barrier(i32 mem_flags, i32 scope): mem_flags bit 0 = device, 1 = threadgroup, 2 = texture; scope 1 = threadgroup, 4 = simdgroup [INFER from
		// the Metal enums; the values seen in the corpus are (2,1) and (2,4)]. A threadgroup barrier = release fence, s_barrier, acquire fence.
		const bool wg = fam == "air.wg.barrier";
		auto *flags = dyn_cast<ConstantInt>(A[0]);
		const bool device = !flags || (flags->getZExtValue() & 1);
		SyncScope::ID ss = C.getOrInsertSyncScopeID(wg ? (device ? "agent" : "workgroup") : "wavefront");
		if (wg)
			B.CreateFence(AtomicOrdering::Release, ss);
		if (wg)
			B.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, ArrayRef<Value *>{});
		B.CreateFence(wg ? AtomicOrdering::Acquire : AtomicOrdering::AcquireRelease, ss);
		return CI;   // void: sentinel
	}
	if (op == "simd_sum") {
		Type *T = A[0]->getType();
		if (T->isIntegerTy(32))
			return B.CreateIntrinsic(Intrinsic::amdgcn_wave_reduce_add, {T}, {A[0], B.getInt32(0)});
		if (T->isFloatTy())
			return B.CreateIntrinsic(Intrinsic::amdgcn_wave_reduce_fadd, {T}, {A[0], B.getInt32(0)});
		*why = "simd_sum on a type without a wave-reduce rule";
		return nullptr;
	}
	if (op == "simd_is_first") {   // first ACTIVE lane
		Value *ballot = B.CreateIntrinsic(Intrinsic::amdgcn_ballot, {B.getInt32Ty()}, {B.getTrue()});
		Value *below = B.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, ArrayRef<Value *>{ballot, B.getInt32(0)});
		return B.CreateICmpEQ(below, B.getInt32(0));
	}
	if (op == "get_simdgroup_size")
		return ConstantInt::get(RT, 32);
	*why = "no rule";
	return nullptr;
}

// Replace every lowerable air.* call of F in place; record what was and was not lowered.
void lowerFunction(Function &F, Stats &st) {
	SmallVector<CallInst *, 32> calls;
	for (Instruction &I : instructions(F))
		if (auto *CI = dyn_cast<CallInst>(&I))
			if (Function *callee = CI->getCalledFunction())
				if (callee->getName().starts_with("air."))
					calls.push_back(CI);
	for (CallInst *CI : calls) {
		StringRef name = CI->getCalledFunction()->getName();
		IRBuilder<> B(CI);
		std::string why;
		Value *v = lowerCall(B, CI, name, &why);
		if (!v) {
			st.unsupported[name.str() + (why == "no rule" ? "" : "  [" + why + "]")]++;
			continue;
		}
		st.lowered[family(name)]++;
		if (v != CI) {
			CI->replaceAllUsesWith(v);
		}
		CI->eraseFromParent();
	}
}

// ---- the kernel ABI ---------------------------------------------------------------------------------------------------------------------------

struct ArgInfo {
	std::string kind;       // air.buffer, air.thread_position_in_grid, ...
	unsigned aspace = 0;
	std::string name;
};

std::string str(const Metadata *m) {
	if (auto *s = dyn_cast_or_null<MDString>(m))
		return s->getString().str();
	return "";
}

ArgInfo parseArg(const MDNode *n) {
	ArgInfo a;
	// !{i32 index, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, ..., !"air.arg_name", !"x"}
	a.kind = str(n->getOperand(1));
	for (unsigned i = 2; i + 1 < n->getNumOperands(); i++) {
		const std::string k = str(n->getOperand(i));
		if (k == "air.address_space")
			if (auto *c = mdconst::dyn_extract<ConstantInt>(n->getOperand(i + 1)))
				a.aspace = c->getZExtValue();
		if (k == "air.arg_name")
			a.name = str(n->getOperand(i + 1));
	}
	return a;
}

// AIR address spaces: 1 device = AMDGPU global (1), 3 threadgroup = AMDGPU local (3), 2 constant = AMDGPU constant (4) (AMDGPU's own 2 is the region/GDS space).
// Only the constant space changes number. LLVM cannot change a pointer's address space in place, so the module is printed, "addrspace(2)" and the
// ".p2" in intrinsic names are rewritten, and it is parsed again: every pointer, GEP, load, global, argument and declaration changes consistently.
static void replaceAll(std::string &s, const std::string &from, const std::string &to) {
	for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
		s.replace(p, from.size(), to);
}

std::unique_ptr<Module> remapConstantSpace(std::unique_ptr<Module> M, LLVMContext &C, std::string &err) {
	std::string text;
	raw_string_ostream os(text);
	M->print(os, nullptr);
	os.flush();
	replaceAll(text, "addrspace(2)", "addrspace(4)");
	replaceAll(text, ".p2.", ".p4.");
	replaceAll(text, ".p2(", ".p4(");
	SMDiagnostic d;
	auto R = parseAssemblyString(text, d, C);
	if (!R)
		err = "re-parse after the address-space rewrite failed: " + d.getMessage().str();
	return R;
}

bool lowerKernel(Module &M, const std::string &entryName, unsigned tg[3], std::string &err, Stats &st) {
	LLVMContext &C = M.getContext();
	NamedMDNode *nk = M.getNamedMetadata("air.kernel");
	if (!nk) {
		err = M.getNamedMetadata("air.vertex") ? "a vertex function, not a compute kernel" : M.getNamedMetadata("air.fragment") ? "a fragment function, not a compute kernel" : "no air.kernel metadata";
		return false;
	}
	MDNode *entry = nullptr;
	Function *F = nullptr;
	for (MDNode *n : nk->operands()) {
		auto *f = mdconst::dyn_extract<Function>(n->getOperand(0));
		if (f && (entryName.empty() || f->getName() == entryName)) {
			entry = n;
			F = f;
			break;
		}
	}
	if (!F) {
		err = "entry '" + entryName + "' not found in air.kernel";
		return false;
	}
	auto *argsMD = cast<MDNode>(entry->getOperand(2));
	std::vector<ArgInfo> infos;
	for (const MDOperand &o : argsMD->operands())
		infos.push_back(parseArg(cast<MDNode>(o)));
	if (infos.size() != F->arg_size()) {
		err = "argument metadata count != function arguments";
		return false;
	}
	// 1. air.* calls in the body (before the type-changing clone, so the callees' declarations keep their AIR types)
	lowerFunction(*F, st);
	if (!st.unsupported.empty()) {
		err = "unsupported air.* call: " + st.unsupported.begin()->first;
		return false;
	}
	// 2. new signature: buffers, then the hidden grid size
	Type *i32 = Type::getInt32Ty(C);
	std::vector<Type *> params;
	for (size_t i = 0; i < infos.size(); i++) {
		if (infos[i].kind == "air.buffer") {
			if (infos[i].aspace != kAirDevice && infos[i].aspace != kAirConstant) {
				err = "buffer argument " + infos[i].name + " in AIR address space " + std::to_string(infos[i].aspace);
				return false;
			}
			params.push_back(PointerType::get(C, infos[i].aspace == kAirDevice ? kAmdGlobal : kAmdConstant));
		}
	}
	const size_t nBuf = params.size();
	for (int d = 0; d < 3; d++)
		params.push_back(i32);
	auto *NFT = FunctionType::get(Type::getVoidTy(C), params, false);
	Function *NF = Function::Create(NFT, GlobalValue::ExternalLinkage, "", M);
	NF->setCallingConv(CallingConv::AMDGPU_KERNEL);
	const char *dimName[3] = {"x", "y", "z"};
	{
		size_t bi = 0;
		for (size_t i = 0; i < infos.size(); i++)
			if (infos[i].kind == "air.buffer")
				NF->getArg(bi++)->setName(infos[i].name.empty() ? "buf" + std::to_string(i) : infos[i].name);
		for (int d = 0; d < 3; d++)
			NF->getArg(nBuf + d)->setName(std::string("air.grid.") + dimName[d]);
	}
	// 3. prologue block: ids, builtins, out-of-grid exit
	BasicBlock *pro = BasicBlock::Create(C, "air.prologue", NF);
	IRBuilder<> B(pro);
	Value *wg[3], *lid[3], *grid[3];
	Intrinsic::ID wgId[3] = {Intrinsic::amdgcn_workgroup_id_x, Intrinsic::amdgcn_workgroup_id_y, Intrinsic::amdgcn_workgroup_id_z};
	Intrinsic::ID liId[3] = {Intrinsic::amdgcn_workitem_id_x, Intrinsic::amdgcn_workitem_id_y, Intrinsic::amdgcn_workitem_id_z};
	for (int d = 0; d < 3; d++) {
		wg[d] = B.CreateIntrinsic(wgId[d], ArrayRef<Value *>{});
		lid[d] = B.CreateIntrinsic(liId[d], ArrayRef<Value *>{});
		grid[d] = NF->getArg(nBuf + d);
	}
	Value *gid[3], *tgSize[3], *ngroups[3];
	for (int d = 0; d < 3; d++) {
		tgSize[d] = B.getInt32(tg[d]);
		gid[d] = B.CreateAdd(B.CreateMul(wg[d], tgSize[d]), lid[d]);
		ngroups[d] = B.CreateUDiv(B.CreateAdd(grid[d], B.getInt32(tg[d] - 1)), tgSize[d]);
	}
	Value *flat = B.CreateAdd(lid[0], B.CreateMul(B.getInt32(tg[0]), B.CreateAdd(lid[1], B.CreateMul(B.getInt32(tg[1]), lid[2]))));
	Value *lane = B.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, ArrayRef<Value *>{B.getInt32(-1), B.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, ArrayRef<Value *>{B.getInt32(-1), B.getInt32(0)})});

	auto build3 = [&](Value *v[3], Type *T) -> Value * {   // <3 x i32> or <2 x i32> or i32 (or the i16 forms) from the three dimensions
		Type *el = T->getScalarType();
		auto cast = [&](Value *x) { return el->isIntegerTy(32) ? x : B.CreateTrunc(x, el); };
		if (auto *VT = dyn_cast<FixedVectorType>(T)) {
			Value *r = PoisonValue::get(VT);
			for (unsigned i = 0; i < VT->getNumElements(); i++)
				r = B.CreateInsertElement(r, cast(v[i]), i);
			return r;
		}
		return cast(v[0]);
	};
	ValueToValueMapTy VMap;
	size_t bi = 0;
	for (size_t i = 0; i < infos.size(); i++) {
		const ArgInfo &a = infos[i];
		Argument *old = F->getArg(i);
		Value *nv = nullptr;
		if (a.kind == "air.buffer") {
			nv = NF->getArg(bi++);
		} else if (a.kind == "air.thread_position_in_grid") {
			nv = build3(gid, old->getType());
		} else if (a.kind == "air.threadgroup_position_in_grid") {
			nv = build3(wg, old->getType());
		} else if (a.kind == "air.thread_position_in_threadgroup") {
			nv = build3(lid, old->getType());
		} else if (a.kind == "air.threads_per_threadgroup") {
			nv = build3(tgSize, old->getType());
		} else if (a.kind == "air.threadgroups_per_grid") {
			nv = build3(ngroups, old->getType());
		} else if (a.kind == "air.threads_per_grid") {
			nv = build3(grid, old->getType());
		} else if (a.kind == "air.thread_index_in_threadgroup") {
			nv = B.CreateZExtOrTrunc(flat, old->getType());
		} else if (a.kind == "air.thread_index_in_simdgroup") {
			nv = B.CreateZExtOrTrunc(lane, old->getType());
		} else if (a.kind == "air.simdgroup_index_in_threadgroup") {
			nv = B.CreateZExtOrTrunc(B.CreateLShr(flat, 5), old->getType());
		} else {
			err = "argument '" + a.name + "' of kind " + a.kind + " has no lowering yet";
			NF->eraseFromParent();
			return false;
		}
		VMap[old] = nv;
	}
	Value *oob = B.getFalse();
	for (int d = 0; d < 3; d++)
		oob = B.CreateOr(oob, B.CreateICmpUGE(gid[d], grid[d]));
	// 4. clone the body into the new signature (the constant address space is already rewritten, see remapConstantSpace)
	SmallVector<ReturnInst *, 8> rets;
	CloneFunctionInto(NF, F, VMap, CloneFunctionChangeType::GlobalChanges, rets, "", nullptr, nullptr);
	BasicBlock *body = cast<BasicBlock>(VMap[&F->getEntryBlock()]);
	BasicBlock *exitBB = BasicBlock::Create(C, "air.exit", NF);
	ReturnInst::Create(C, exitBB);
	B.SetInsertPoint(pro);
	B.CreateCondBr(oob, exitBB, body);
	// 5. attributes the AMDGPU back end wants
	std::string name = F->getName().str();
	nk->eraseFromParent();
	F->eraseFromParent();
	NF->setName(name);
	NF->setCallingConv(CallingConv::AMDGPU_KERNEL);   // CloneFunctionInto copied the AIR (C) convention
	for (unsigned i = 0; i < NF->arg_size(); i++)
		NF->removeParamAttr(i, "air-buffer-no-alias");
	NF->removeFnAttr("frame-pointer");
	NF->removeFnAttr(Attribute::Convergent);
	NF->addFnAttr("amdgpu-flat-work-group-size", std::to_string(tg[0] * tg[1] * tg[2]) + "," + std::to_string(tg[0] * tg[1] * tg[2]));
	NF->setMetadata("reqd_work_group_size", MDNode::get(C, {ConstantAsMetadata::get(B.getInt32(tg[0])), ConstantAsMetadata::get(B.getInt32(tg[1])), ConstantAsMetadata::get(B.getInt32(tg[2]))}));
	NF->addFnAttr("denormal-fp-math-f32", "preserve-sign,preserve-sign");   // air.compile.denorms_disable
	// llc does not run AMDGPU's attributor: say what this ABI never uses, or the kernel asks the CP for dispatch/queue pointers and an
	// implicit-arg block (user SGPRs 8 instead of 2). Workgroup ids, workitem ids and the kernarg pointer are the only inputs.
	for (const char *a : {"amdgpu-no-dispatch-ptr", "amdgpu-no-queue-ptr", "amdgpu-no-dispatch-id", "amdgpu-no-implicitarg-ptr", "amdgpu-no-heap-ptr",
	                      "amdgpu-no-hostcall-ptr", "amdgpu-no-multigrid-sync-arg", "amdgpu-no-completion-action", "amdgpu-no-default-queue",
	                      "amdgpu-no-lds-kernel-id"})
		NF->addFnAttr(a);
	NF->addFnAttr("uniform-work-group-size", "false");
	return true;
}

// ---- driver -------------------------------------------------------------------------------------------------------------------------------------

std::unique_ptr<TargetMachine> makeTM(const std::string &cpu) {
	std::string e;
	const Target *T = TargetRegistry::lookupTarget(Triple("amdgcn-amd-amdhsa"), e);
	if (!T) {
		errs() << "no AMDGPU target in this LLVM: " << e << "\n";
		return nullptr;
	}
	TargetOptions opt;
	return std::unique_ptr<TargetMachine>(T->createTargetMachine(Triple("amdgcn-amd-amdhsa"), cpu, "", opt, Reloc::PIC_, std::nullopt, CodeGenOptLevel::Default));
}

void retarget(Module &M, TargetMachine &TM) {
	M.setTargetTriple(Triple("amdgcn-amd-amdhsa"));
	M.setDataLayout(TM.createDataLayout());
	SmallVector<NamedMDNode *, 8> dead;
	for (NamedMDNode &n : M.named_metadata())
		if (n.getName().starts_with("air."))
			dead.push_back(&n);
	for (NamedMDNode *n : dead)
		n->eraseFromParent();
	// drop the AIR module flags (SDK Version, air.max_*)
	if (NamedMDNode *flags = M.getModuleFlagsMetadata()) {
		SmallVector<MDNode *, 8> keep;
		for (MDNode *f : flags->operands()) {
			const std::string k = str(f->getOperand(1));
			if (k.rfind("air.", 0) != 0 && k != "SDK Version")
				keep.push_back(f);
		}
		flags->clearOperands();
		for (MDNode *f : keep)
			flags->addOperand(f);
	}
}

std::string jsonEscape(const std::string &s) {
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\')
			o += '\\';
		if ((unsigned char)c < 0x20)
			continue;
		o += c;
	}
	return o;
}

struct DiagCollector {
	std::string text;
	bool failed = false;
};

} // namespace

int main(int argc, char **argv) {
	llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
	LLVMInitializeAMDGPUTargetInfo();
	LLVMInitializeAMDGPUTarget();
	LLVMInitializeAMDGPUTargetMC();
	LLVMInitializeAMDGPUAsmPrinter();
	if (argc < 3) {
		fprintf(stderr, "usage: %s kernel IN --tg X,Y,Z -o OUT.ll [--entry NAME] [--mcpu gfx1201]\n       %s coverage [--codegen] IN...\n", argv[0], argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	std::string out, entry, cpu = "gfx1201", tgs = "64,1,1";
	bool codegen = false;
	std::vector<std::string> inputs;
	for (int i = 2; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "-o" && i + 1 < argc)
			out = argv[++i];
		else if (a == "--tg" && i + 1 < argc)
			tgs = argv[++i];
		else if (a == "--entry" && i + 1 < argc)
			entry = argv[++i];
		else if (a == "--mcpu" && i + 1 < argc)
			cpu = argv[++i];
		else if (a == "--codegen")
			codegen = true;
		else
			inputs.push_back(a);
	}
	auto TM = makeTM(cpu);
	if (!TM)
		return 2;

	if (mode == "kernel") {
		unsigned tg[3] = {1, 1, 1};
		if (sscanf(tgs.c_str(), "%u,%u,%u", &tg[0], &tg[1], &tg[2]) < 1 || tg[0] * tg[1] * tg[2] == 0 || tg[0] * tg[1] * tg[2] > 1024) {
			fprintf(stderr, "--tg X,Y,Z with X*Y*Z in 1..1024\n");
			return 2;
		}
		if (inputs.size() != 1 || out.empty()) {
			fprintf(stderr, "kernel mode: one input and -o\n");
			return 2;
		}
		LLVMContext ctx;
		SMDiagnostic d;
		auto M = parseIRFile(inputs[0], d, ctx);
		if (!M) {
			d.print(argv[0], errs());
			return 1;
		}
		Stats st;
		std::string err;
		M = remapConstantSpace(std::move(M), ctx, err);
		if (!M || !lowerKernel(*M, entry, tg, err, st)) {
			fprintf(stderr, "air2amdgcn: %s: %s\n", inputs[0].c_str(), err.c_str());
			for (auto &u : st.unsupported)
				fprintf(stderr, "  unsupported: %s (x%u)\n", u.first.c_str(), u.second);
			return 1;
		}
		retarget(*M, *TM);
		if (verifyModule(*M, &errs())) {
			fprintf(stderr, "air2amdgcn: the lowered module does not verify\n");
			return 1;
		}
		std::error_code ec;
		raw_fd_ostream os(out, ec);
		M->print(os, nullptr);
		fprintf(stderr, "air2amdgcn: lowered");
		for (auto &l : st.lowered)
			fprintf(stderr, " %s x%u", l.first.c_str(), l.second);
		fprintf(stderr, "%s\n", st.lowered.empty() ? " (no air.* calls)" : "");
		return 0;
	}

	if (mode == "coverage") {
		for (const std::string &in : inputs) {
			LLVMContext ctx;
			DiagCollector dc;
			ctx.setDiagnosticHandlerCallBack([](const DiagnosticInfo *DI, void *p) {
				auto *c = static_cast<DiagCollector *>(p);
				if (DI->getSeverity() == DS_Error) {
					c->failed = true;
					raw_string_ostream os(c->text);
					DiagnosticPrinterRawOStream pr(os);
					DI->print(pr);
				}
			}, &dc);
			SMDiagnostic d;
			auto M = parseIRFile(in, d, ctx);
			if (!M) {
				printf("{\"file\":\"%s\",\"parse\":false}\n", jsonEscape(in).c_str());
				continue;
			}
			Stats st;
			std::string rerr;
			M = remapConstantSpace(std::move(M), ctx, rerr);
			if (!M) {
				printf("{\"file\":\"%s\",\"parse\":false,\"error\":\"%s\"}\n", jsonEscape(in).c_str(), jsonEscape(rerr).substr(0, 200).c_str());
				continue;
			}
			for (Function &F : *M)
				if (!F.isDeclaration())
					lowerFunction(F, st);
			std::string json = "{\"file\":\"" + jsonEscape(in) + "\",\"lowered\":{";
			bool first = true;
			for (auto &l : st.lowered) {
				json += (first ? "" : ",") + std::string("\"") + l.first + "\":" + std::to_string(l.second);
				first = false;
			}
			json += "},\"unsupported\":{";
			first = true;
			for (auto &l : st.unsupported) {
				json += (first ? "" : ",") + std::string("\"") + jsonEscape(l.first) + "\":" + std::to_string(l.second);
				first = false;
			}
			json += "}";
			if (codegen && st.unsupported.empty()) {
				retarget(*M, *TM);
				for (Function &F : *M)
					if (!F.isDeclaration()) {
						F.setCallingConv(CallingConv::AMDGPU_Gfx);
						F.removeFnAttr("frame-pointer");
					}
				SmallVector<char, 0> buf;
				raw_svector_ostream bos(buf);
				legacy::PassManager pm;
				if (TM->addPassesToEmitFile(pm, bos, nullptr, CodeGenFileType::ObjectFile))
					dc.failed = true, dc.text = "cannot emit";
				else
					pm.run(*M);
				json += std::string(",\"codegen\":") + (dc.failed ? "false" : "true");
				if (dc.failed)
					json += ",\"codegen_error\":\"" + jsonEscape(dc.text).substr(0, 300) + "\"";
			}
			json += "}";
			printf("%s\n", json.c_str());
			fflush(stdout);
		}
		return 0;
	}
	fprintf(stderr, "mode must be kernel or coverage\n");
	return 2;
}
