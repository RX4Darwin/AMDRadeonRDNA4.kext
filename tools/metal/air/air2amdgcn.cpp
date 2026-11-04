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
//   * then one hidden u32 per threadgroup-memory argument (AIR buffer in address space 3): its byte offset into the dynamic LDS (the runtime sums the
//     setThreadgroupMemoryLength sizes, 16-byte aligned); the kernel's dynamic LDS starts after its static LDS (group_segment_fixed_size);
//   * threadgroup size is a compile-time constant (--tg), as in a Metal pipeline (maxTotalThreadsPerThreadgroup / dispatch size are known at
//     pipeline creation); the launch must use groups = ceil(grid / tg) per dimension; threads outside the grid return at once (Metal has no such
//     threads: non-uniform threadgroups), which is safe before barriers because the hardware counts waves, not lanes;
//   * thread_position_in_grid / threadgroup_position_in_grid / thread_position_in_threadgroup / thread_index_in_threadgroup /
//     threads_per_threadgroup / threadgroups_per_grid / threads_per_grid / thread_index_in_simdgroup / simdgroup_index_in_threadgroup are
//     computed in a prologue from workgroup id, workitem id and the hidden grid size; a simdgroup is one wave32;
//   * threadgroup memory = the module's addrspace(3) globals (static LDS), unchanged.
//   --abi FILE writes the kernargs in order, one line each: "buffer SLOT", "grid", "threadgroup SLOT" (SLOT = the argument's [[buffer(n)]]).
//
//   air2amdgcn vertex   IN -o OUT.ll --abi OUT.abi [--vertex-desc DESC]       a vertex function as a kernel that writes its outputs per vertex
//   air2amdgcn fragment IN --vertex-abi V.abi -o OUT.ll --abi OUT.abi         a fragment function inside a kernel that rasterizes the draw
//   (render stages as compute: see lowerVertex/lowerFragment; the fragment kernel is linked with rdna4raster.ll)
#include <fstream>
#include <sstream>
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
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

#include <algorithm>
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
	std::vector<std::string> abi;                  // kernel mode: the kernargs, one line each (--abi)
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
		Value *r = fn(B, e);
		if (!r)
			return nullptr;   // no rule for this element type
		res = B.CreateInsertElement(res, r, i);
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

// A 32-bit float of the lane `lane` (ds_bpermute; the lanes must be active).
Value *laneValue(IRBuilder<> &B, Value *v, Value *lane) {
	Value *i = B.CreateBitCast(v, B.getInt32Ty());
	Value *r = B.CreateIntrinsic(B.getInt32Ty(), Intrinsic::amdgcn_ds_bpermute, {B.CreateShl(lane, 2), i});
	return B.CreateBitCast(r, B.getFloatTy());
}

// The difference across the pixel's 2x2 quad of a float: the fragment kernel's lanes are the rows of its 8x8 tile, so the neighbour in x is
// lane ^ 1 and in y lane ^ 8 (`bit`); coarse, the quad's right (lower) pixel less its left (upper) one.
Value *quadDifference(IRBuilder<> &B, Value *v, unsigned bit) {
	Type *T = v->getType();
	Value *lane = B.CreateIntrinsic(B.getInt32Ty(), Intrinsic::amdgcn_mbcnt_lo, {B.getInt32(~0u), B.getInt32(0)});
	Value *f = B.CreateFPExt(v, B.getFloatTy());
	Value *hi = laneValue(B, f, B.CreateOr(lane, B.getInt32(bit)));
	Value *lo = laneValue(B, f, B.CreateAnd(lane, B.getInt32(~bit)));
	return B.CreateFPTrunc(B.CreateFSub(hi, lo), T);
}

// atan2(y, x) of floats, a minimax polynomial of atan on [0, 1] folded into the octants (error about 1e-5 rad).
Value *atan2Approx(IRBuilder<> &B, Value *y, Value *x) {
	Type *T = y->getType();
	Value *f = B.CreateFPExt(y, B.getFloatTy()), *g = B.CreateFPExt(x, B.getFloatTy());
	Value *ay = B.CreateUnaryIntrinsic(Intrinsic::fabs, f), *ax = B.CreateUnaryIntrinsic(Intrinsic::fabs, g);
	Value *hi = B.CreateBinaryIntrinsic(Intrinsic::maxnum, ax, ay), *lo = B.CreateBinaryIntrinsic(Intrinsic::minnum, ax, ay);
	auto c = [&](double v) { return ConstantFP::get(B.getFloatTy(), v); };
	Value *a = B.CreateSelect(B.CreateFCmpOEQ(hi, c(0)), c(0), B.CreateFDiv(lo, hi));
	Value *s = B.CreateFMul(a, a);
	Value *r = B.CreateFAdd(B.CreateFMul(c(-0.0464964749), s), c(0.15931422));
	r = B.CreateFAdd(B.CreateFMul(r, s), c(-0.327622764));
	r = B.CreateFAdd(B.CreateFMul(B.CreateFMul(r, s), a), a);
	r = B.CreateSelect(B.CreateFCmpOGT(ay, ax), B.CreateFSub(c(1.57079637), r), r);
	r = B.CreateSelect(B.CreateFCmpOLT(g, c(0)), B.CreateFSub(c(3.14159274), r), r);
	r = B.CreateSelect(B.CreateFCmpOLT(f, c(0)), B.CreateFNeg(r), r);
	return B.CreateFPTrunc(r, T);
}

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
	if (op == "dfdx" || op == "dfdy") {   // the fragment kernel's quads (lowerFragment)
		const unsigned bit = op == "dfdx" ? 1 : 8;
		return elementwise(B, RT, {A[0]}, [&](IRBuilder<> &B, ArrayRef<Value *> e) { return quadDifference(B, e[0], bit); });
	}
	if (op == "fwidth")
		return elementwise(B, RT, {A[0]}, [&](IRBuilder<> &B, ArrayRef<Value *> e) {
			return B.CreateFAdd(B.CreateUnaryIntrinsic(Intrinsic::fabs, quadDifference(B, e[0], 1)),
			                    B.CreateUnaryIntrinsic(Intrinsic::fabs, quadDifference(B, e[0], 8)));
		});
	if (op == "pack.unorm4x8") {   // float4 in [0, 1] -> 4 x u8, x lowest, rounded to nearest
		Value *r = B.getInt32(0);
		for (unsigned i = 0; i < 4; i++) {
			Value *c = B.CreateExtractElement(A[0], i);
			c = B.CreateFPExt(c, B.getFloatTy());
			c = B.CreateBinaryIntrinsic(Intrinsic::minnum, B.CreateBinaryIntrinsic(Intrinsic::maxnum, c, ConstantFP::get(B.getFloatTy(), 0.0)),
			                            ConstantFP::get(B.getFloatTy(), 1.0));
			c = B.CreateFPToUI(B.CreateFAdd(B.CreateFMul(c, ConstantFP::get(B.getFloatTy(), 255.0)), ConstantFP::get(B.getFloatTy(), 0.5)),
			                   B.getInt32Ty());
			r = B.CreateOr(r, B.CreateShl(c, 8 * i));
		}
		return r;
	}
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
	if (op == "atan2")
		return elementwise(B, RT, {A[0], A[1]}, [&](IRBuilder<> &B, ArrayRef<Value *> e) { return atan2Approx(B, e[0], e[1]); });
	if (op == "atan")
		return elementwise(B, RT, {A[0]}, [&](IRBuilder<> &B, ArrayRef<Value *> e) {
			return atan2Approx(B, e[0], ConstantFP::get(e[0]->getType(), 1.0));
		});
	if (op == "sin")
		return fm(un(Intrinsic::sin));
	if (op == "cos")
		return fm(un(Intrinsic::cos));
	if (op == "fmax3" || op == "fmin3") {
		const Intrinsic::ID id = op == "fmax3" ? Intrinsic::maxnum : Intrinsic::minnum;
		return B.CreateBinaryIntrinsic(id, B.CreateBinaryIntrinsic(id, A[0], A[1]), A[2]);
	}
	if (fam == "air.unpack.unorm4x8") {   // u32 -> float4 (or half4): byte / 255
		Value *bytes = B.CreateBitCast(A[0], FixedVectorType::get(B.getInt8Ty(), 4));
		return B.CreateFMul(B.CreateUIToFP(bytes, RT), fconst(B, RT, 1.0 / 255.0));
	}
	if (fam.rfind("air.atomic.", 0) == 0) {
		// air.atomic.<global|local>.<op>[.s|.u].<type>(ptr, [value,] i32 order, i32 scope, i1 volatile); order 0 (relaxed) is the only value in the corpus; scope 1 =
		// threadgroup, 2 = device [INFER from the values seen: local atomics carry 1, global ones 2]
		const std::string aop = tok.size() > 3 ? tok[3].str() : "";
		const bool isLoad = aop == "load", isStore = aop == "store";
		auto *scopeC = dyn_cast<ConstantInt>(A[A.size() - 2]);
		auto *orderC = dyn_cast<ConstantInt>(A[A.size() - 3]);
		if (!scopeC || !orderC || orderC->getZExtValue() != 0) {
			*why = "atomic with a non-relaxed or non-constant ordering";
			return nullptr;
		}
		SyncScope::ID ss = C.getOrInsertSyncScopeID(scopeC->getZExtValue() == 1 ? "workgroup" : scopeC->getZExtValue() == 2 ? "agent" : "wavefront");
		const bool sgn = signOf(tok) == 's';
		if (isLoad) {
			auto *L = B.CreateAlignedLoad(RT, A[0], Align(RT->getPrimitiveSizeInBits() / 8));
			L->setAtomic(AtomicOrdering::Monotonic, ss);
			return L;
		}
		if (isStore) {
			auto *St = B.CreateAlignedStore(A[1], A[0], Align(A[1]->getType()->getPrimitiveSizeInBits() / 8));
			St->setAtomic(AtomicOrdering::Monotonic, ss);
			return CI;
		}
		AtomicRMWInst::BinOp bop;
		if (aop == "add") bop = AtomicRMWInst::Add;
		else if (aop == "sub") bop = AtomicRMWInst::Sub;
		else if (aop == "and") bop = AtomicRMWInst::And;
		else if (aop == "or") bop = AtomicRMWInst::Or;
		else if (aop == "xor") bop = AtomicRMWInst::Xor;
		else if (aop == "min") bop = sgn ? AtomicRMWInst::Min : AtomicRMWInst::UMin;
		else if (aop == "max") bop = sgn ? AtomicRMWInst::Max : AtomicRMWInst::UMax;
		else if (aop == "exchange") bop = AtomicRMWInst::Xchg;
		else {
			*why = "atomic op without a rule (" + aop + ")";
			return nullptr;
		}
		return B.CreateAtomicRMW(bop, A[0], A[1], MaybeAlign(), AtomicOrdering::Monotonic, ss);
	}
	if (op == "is_uniform") {   // all active lanes hold the same value
		Value *x = A[0]->getType()->isIntegerTy(32) ? A[0] : B.CreateZExtOrTrunc(A[0], B.getInt32Ty());
		Value *first = B.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {B.getInt32Ty()}, {x});
		Value *differs = B.CreateICmpNE(x, first);
		Value *mask = B.CreateIntrinsic(Intrinsic::amdgcn_ballot, {B.getInt32Ty()}, {differs});
		return B.CreateICmpEQ(mask, B.getInt32(0));
	}
	if (op == "tanh") {   // (e - 1) / (e + 1), e = exp(2x), |x| clamped so that e stays finite
		Value *x = B.CreateBinaryIntrinsic(Intrinsic::minnum, B.CreateBinaryIntrinsic(Intrinsic::maxnum, A[0], fconst(B, RT, -9.0)), fconst(B, RT, 9.0));
		Value *e = B.CreateUnaryIntrinsic(Intrinsic::exp, B.CreateFMul(x, fconst(B, RT, 2.0)));
		return B.CreateFDiv(B.CreateFSub(e, fconst(B, RT, 1.0)), B.CreateFAdd(e, fconst(B, RT, 1.0)));
	}
	if (op == "popcount")
		return B.CreateUnaryIntrinsic(Intrinsic::ctpop, A[0]);
	if (op == "fmod")
		return B.CreateFRem(A[0], A[1]);
	if (op == "simd_max" || op == "simd_min") {
		const bool mx = op == "simd_max";
		return elementwise(B, RT, A, [&](IRBuilder<> &b, ArrayRef<Value *> e) -> Value * {
			Type *T = e[0]->getType();
			if (T->isFloatTy())
				return b.CreateIntrinsic(mx ? Intrinsic::amdgcn_wave_reduce_fmax : Intrinsic::amdgcn_wave_reduce_fmin, {T}, {e[0], b.getInt32(0)});
			if (T->isIntegerTy(32)) {
				const bool sg = signOf(tok) == 's';
				return b.CreateIntrinsic(mx ? (sg ? Intrinsic::amdgcn_wave_reduce_max : Intrinsic::amdgcn_wave_reduce_umax)
				                            : (sg ? Intrinsic::amdgcn_wave_reduce_min : Intrinsic::amdgcn_wave_reduce_umin), {T}, {e[0], b.getInt32(0)});
			}
			return nullptr;
		});
	}
	if (op == "simd_shuffle" || op == "simd_shuffle_down" || op == "simd_shuffle_up" || op == "simd_shuffle_xor") {
		// ds_bpermute: lane l reads the value of lane idx (byte address idx*4); out-of-range deltas read a wrapped lane (Metal: undefined)
		return elementwise(B, RT, A, [&](IRBuilder<> &b, ArrayRef<Value *> e) -> Value * {
			Type *T = e[0]->getType();
			if (!T->isFloatTy() && !T->isIntegerTy(32) && !T->isIntegerTy(16) && !T->isHalfTy())
				return nullptr;
			Value *lane = b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, ArrayRef<Value *>{b.getInt32(-1), b.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, ArrayRef<Value *>{b.getInt32(-1), b.getInt32(0)})});
			Value *d = b.CreateZExtOrTrunc(e[1], b.getInt32Ty());
			Value *src = op == "simd_shuffle" ? d : op == "simd_shuffle_down" ? b.CreateAdd(lane, d) : op == "simd_shuffle_up" ? b.CreateSub(lane, d) : b.CreateXor(lane, d);
			src = b.CreateAnd(src, b.getInt32(31));
			Value *as32 = T->isFloatTy() ? b.CreateBitCast(e[0], b.getInt32Ty()) : T->isIntegerTy(32) ? e[0] : b.CreateZExt(T->isHalfTy() ? b.CreateBitCast(e[0], b.getInt16Ty()) : e[0], b.getInt32Ty());
			Value *r = b.CreateIntrinsic(Intrinsic::amdgcn_ds_bpermute, ArrayRef<Value *>{b.CreateShl(src, 2), as32});
			if (T->isFloatTy()) return b.CreateBitCast(r, T);
			if (T->isIntegerTy(32)) return r;
			Value *t16 = b.CreateTrunc(r, b.getInt16Ty());
			return T->isHalfTy() ? b.CreateBitCast(t16, T) : t16;
		});
	}
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
	if (op == "simd_sum" && A[0]->getType()->isVectorTy())
		return elementwise(B, RT, A, [&](IRBuilder<> &b, ArrayRef<Value *> e) -> Value * {
			if (!e[0]->getType()->isFloatTy())   // wave.reduce.fadd.f16 cannot be selected on gfx1201 (LLVM 22)
				return nullptr;
			return b.CreateIntrinsic(Intrinsic::amdgcn_wave_reduce_fadd, {e[0]->getType()}, {e[0], b.getInt32(0)});
		});
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
		if (!v && why.empty())
			why = "no rule";
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
	unsigned slot = 0;      // air.location_index: the [[buffer(n)]] it binds
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
		if (k == "air.location_index")
			if (auto *c = mdconst::dyn_extract<ConstantInt>(n->getOperand(i + 1)))
				a.slot = c->getZExtValue();
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
	// AIR spaces 4 (imageblock/tile memory, as returned by air.imageblock_data), 5, 6, 7 have no lowering and would collide with AMDGPU's numbering
	for (const char *sp : {"addrspace(4)", "addrspace(5)", "addrspace(6)", "addrspace(7)"})
		if (text.find(sp) != std::string::npos) {
			err = std::string("AIR ") + sp + " (imageblock / tile memory or unknown) is not lowered";
			return nullptr;
		}
	replaceAll(text, "addrspace(2)", "addrspace(4)");
	replaceAll(text, ".p2.", ".p4.");
	replaceAll(text, ".p2(", ".p4(");
	SMDiagnostic d;
	auto R = parseAssemblyString(text, d, C);
	if (!R)
		err = "re-parse after the address-space rewrite failed: " + d.getMessage().str();
	return R;
}

// What every kernel of this compiler declares: its fixed threadgroup size, flushed f32 denormals (air.compile.denorms_disable) and the
// inputs it does without. llc does not run AMDGPU's attributor: say what this ABI never uses, or the kernel asks the CP for dispatch/queue
// pointers and an implicit-arg block (user SGPRs 8 instead of 2). Workgroup ids, workitem ids and the kernarg pointer are the only inputs.
void kernelAttributes(Function *NF, const unsigned tg[3]) {
	LLVMContext &C = NF->getContext();
	auto i32 = [&](unsigned v) { return ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(C), v)); };
	NF->addFnAttr("amdgpu-flat-work-group-size", std::to_string(tg[0] * tg[1] * tg[2]) + "," + std::to_string(tg[0] * tg[1] * tg[2]));
	NF->setMetadata("reqd_work_group_size", MDNode::get(C, {i32(tg[0]), i32(tg[1]), i32(tg[2])}));
	NF->addFnAttr("denormal-fp-math-f32", "preserve-sign,preserve-sign");
	for (const char *a : {"amdgpu-no-dispatch-ptr", "amdgpu-no-queue-ptr", "amdgpu-no-dispatch-id", "amdgpu-no-implicitarg-ptr", "amdgpu-no-heap-ptr",
	                      "amdgpu-no-hostcall-ptr", "amdgpu-no-multigrid-sync-arg", "amdgpu-no-completion-action", "amdgpu-no-default-queue",
	                      "amdgpu-no-lds-kernel-id"})
		NF->addFnAttr(a);
	NF->addFnAttr("uniform-work-group-size", "false");
}

void lowerModule(Module &M, Stats &st, bool textures);

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
	// AIR before 2.3 (a paravirtual device's Metal 2.2) leaves air.address_space out of a buffer's metadata: the
	// pointer type carries it.
	for (size_t i = 0; i < infos.size(); i++) {
		auto *pt = dyn_cast<PointerType>(F->getArg(i)->getType());
		if (infos[i].kind == "air.buffer" && !infos[i].aspace && pt)
			infos[i].aspace = pt->getAddressSpace() == kAmdConstant ? kAirConstant : pt->getAddressSpace();
	}
	// 1. air.* calls in the body and the functions it calls (before the type-changing clone, so the callees' declarations keep their
	// AIR types)
	lowerModule(M, st, false);
	if (!st.unsupported.empty()) {
		err = "unsupported air.* call: " + st.unsupported.begin()->first;
		return false;
	}
	// 2. new signature: buffers, then the hidden grid size
	Type *i32 = Type::getInt32Ty(C);
	std::vector<Type *> params;
	std::vector<size_t> tgArgs;   // threadgroup-memory arguments: a byte offset into the dynamic LDS, passed after the grid size
	for (size_t i = 0; i < infos.size(); i++) {
		if (infos[i].kind == "air.buffer" && infos[i].aspace == kAirThreadgroup) {
			tgArgs.push_back(i);
		} else if (infos[i].kind == "air.buffer") {
			if (infos[i].aspace != kAirDevice && infos[i].aspace != kAirConstant) {
				err = "buffer argument " + infos[i].name + " in AIR address space " + std::to_string(infos[i].aspace);
				return false;
			}
			params.push_back(PointerType::get(C, infos[i].aspace == kAirDevice ? kAmdGlobal : kAmdConstant));
		}
	}
	const size_t nBuf = params.size();
	// The kernargs as a runtime fills them: "buffer SLOT" (8 bytes), "grid" (3 x u32), "threadgroup SLOT" (u32).
	for (size_t i = 0; i < infos.size(); i++)
		if (infos[i].kind == "air.buffer" && infos[i].aspace != kAirThreadgroup)
			st.abi.push_back("buffer " + std::to_string(infos[i].slot));
	st.abi.push_back("grid");
	for (size_t t : tgArgs)
		st.abi.push_back("threadgroup " + std::to_string(infos[t].slot));
	for (int d = 0; d < 3; d++)
		params.push_back(i32);
	for (size_t i = 0; i < tgArgs.size(); i++)
		params.push_back(i32);
	auto *NFT = FunctionType::get(Type::getVoidTy(C), params, false);
	Function *NF = Function::Create(NFT, GlobalValue::ExternalLinkage, "", M);
	NF->setCallingConv(CallingConv::AMDGPU_KERNEL);
	const char *dimName[3] = {"x", "y", "z"};
	{
		size_t bi = 0;
		for (size_t i = 0; i < infos.size(); i++)
			if (infos[i].kind == "air.buffer" && infos[i].aspace != kAirThreadgroup)
				NF->getArg(bi++)->setName(infos[i].name.empty() ? "buf" + std::to_string(i) : infos[i].name);
		for (int d = 0; d < 3; d++)
			NF->getArg(nBuf + d)->setName(std::string("air.grid.") + dimName[d]);
		for (size_t t = 0; t < tgArgs.size(); t++)
			NF->getArg(nBuf + 3 + t)->setName("air.tg." + infos[tgArgs[t]].name + ".offset");
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
		if (a.kind == "air.buffer" && a.aspace == kAirThreadgroup) {
			size_t t = std::find(tgArgs.begin(), tgArgs.end(), i) - tgArgs.begin();
			GlobalVariable *dyn = M.getGlobalVariable("air.dynamic_lds");
			if (!dyn) {
				dyn = new GlobalVariable(M, ArrayType::get(Type::getInt8Ty(C), 0), false, GlobalValue::ExternalLinkage, nullptr, "air.dynamic_lds", nullptr,
				                         GlobalValue::NotThreadLocal, kAmdLocal);
				dyn->setAlignment(Align(16));
			}
			nv = B.CreateGEP(Type::getInt8Ty(C), dyn, {NF->getArg(nBuf + 3 + t)});
		} else if (a.kind == "air.buffer") {
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
		} else if (a.kind == "air.threads_per_simdgroup") {
			nv = ConstantInt::get(old->getType(), 32);
		} else if (a.kind == "air.simdgroups_per_threadgroup") {
			nv = ConstantInt::get(old->getType(), (tg[0] * tg[1] * tg[2] + 31) / 32);
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
	kernelAttributes(NF, tg);
	return true;
}

// ---- render stages: a draw as compute kernels -------------------------------------------------------------------------------------------------
//
// A vertex function becomes a kernel that runs it once per vertex and stores what it returns as a record; a fragment function becomes a
// kernel of one thread per pixel of the render target that rasterizes the draw's triangles from those records (rdna4raster.c, linked in by
// the compiler service) and calls it for every triangle covering the pixel. The AIR function itself stays as it is, internal, and is
// inlined into its kernel.

// One entry of a stage's argument or output list: !{[i32 index,] !"kind", [!"key",] flags...}.
struct StageItem {
	std::string kind, key, name;
	unsigned aspace = 0, slot = 0;
	unsigned interp = 0;    // rdna4raster.c: 0 perspective, 1 linear (air.no_perspective), 2 flat
};

StageItem parseStageItem(const MDNode *n, unsigned first) {
	StageItem a;
	a.kind = str(n->getOperand(first));
	if ((a.kind == "air.vertex_output" || a.kind == "air.fragment_input") && n->getNumOperands() > first + 1)
		a.key = str(n->getOperand(first + 1));
	for (unsigned i = first + 1; i < n->getNumOperands(); i++) {
		const std::string k = str(n->getOperand(i));
		auto num = [&]() -> unsigned {
			if (i + 1 < n->getNumOperands())
				if (auto *c = mdconst::dyn_extract<ConstantInt>(n->getOperand(i + 1)))
					return c->getZExtValue();
			return 0;
		};
		if (k == "air.address_space")
			a.aspace = num();
		else if (k == "air.location_index")
			a.slot = num();
		else if (k == "air.arg_name" && i + 1 < n->getNumOperands())
			a.name = str(n->getOperand(i + 1));
		else if (k == "air.flat")
			a.interp = 2;
		else if (k == "air.no_perspective")
			a.interp = 1;
	}
	return a;
}

// The stage's function and its argument and output lists, from !air.vertex or !air.fragment.
Function *stageEntry(Module &M, const char *md, std::vector<StageItem> &args, std::vector<StageItem> &outs, std::string &err) {
	NamedMDNode *nm = M.getNamedMetadata(md);
	if (!nm || !nm->getNumOperands()) {
		err = std::string("no ") + md + " metadata";
		return nullptr;
	}
	MDNode *e = nm->getOperand(0);
	auto *F = mdconst::dyn_extract<Function>(e->getOperand(0));
	if (!F) {
		err = std::string(md) + " names no function";
		return nullptr;
	}
	for (const MDOperand &o : cast<MDNode>(e->getOperand(1))->operands())
		outs.push_back(parseStageItem(cast<MDNode>(o), 0));
	for (const MDOperand &o : cast<MDNode>(e->getOperand(2))->operands())
		args.push_back(parseStageItem(cast<MDNode>(o), 1));
	if (args.size() != F->arg_size()) {
		err = "argument metadata count != function arguments";
		return nullptr;
	}
	for (size_t i = 0; i < args.size(); i++) {
		auto *pt = dyn_cast<PointerType>(F->getArg(i)->getType());
		// AIR 2.2 names the space only in the pointer type, which remapConstantSpace has already renumbered: AMDGPU's
		// constant space is AIR's
		if (args[i].kind == "air.buffer" && !args[i].aspace && pt)
			args[i].aspace = pt->getAddressSpace() == kAmdConstant ? kAirConstant : pt->getAddressSpace();
		if (args[i].kind == "air.buffer" && args[i].aspace != kAirDevice && args[i].aspace != kAirConstant) {
			err = "buffer argument " + args[i].name + " in AIR address space " + std::to_string(args[i].aspace);
			return nullptr;
		}
	}
	return F;
}

unsigned components(Type *T);

bool isResource(const StageItem &a) {
	return a.kind == "air.buffer" || a.kind == "air.texture" || a.kind == "air.sampler";
}

// The stage's resources as kernel parameters (and ABI lines), in argument order: buffers, and textures and samplers as pointers to what
// the runtime describes them with (rdna4raster.c: an rdna4_texture in global memory, the packed sampler state in constant memory).
void resourceParams(LLVMContext &C, const std::vector<StageItem> &args, std::vector<Type *> &params, Stats &st) {
	for (const StageItem &a : args) {
		if (a.kind == "air.buffer") {
			params.push_back(PointerType::get(C, a.aspace == kAirDevice ? kAmdGlobal : kAmdConstant));
			st.abi.push_back("buffer " + std::to_string(a.slot));
		} else if (a.kind == "air.texture") {
			params.push_back(PointerType::get(C, kAmdGlobal));
			st.abi.push_back("texture " + std::to_string(a.slot));
		} else if (a.kind == "air.sampler") {
			params.push_back(PointerType::get(C, kAmdConstant));
			st.abi.push_back("sampler " + std::to_string(a.slot));
		}
	}
}

// A constant sampler of the function (air.sampler_states: an i64 global) in the encoding rdna4raster.c reads, the guest's packed
// MTLSamplerDescriptor word (docs/m0-pvgpu.md): AIR's has the s/t/r address in bits 0-2/3-5/6-8 in MSL's numbering (clamp_to_zero 0,
// clamp_to_edge 1, repeat 2, mirrored_repeat 3, clamp_to_border 4), the mag and min filters in bits 9-10 and 11-12 and pixel
// coordinates in bit 15 (read from WindowServer's samplers).
uint64_t samplerWord(uint64_t air) {
	static const unsigned address[] = {4, 0, 2, 3, 4, 4, 4, 4};   // MTLSamplerAddressMode
	const unsigned s = address[air & 7], t = address[(air >> 3) & 7];
	const unsigned mag = (air >> 9) & 3, min = (air >> 11) & 3;
	return min | mag << 2 | s << 5 | t << 8 | ((air >> 15) & 1 ? 0u : 1u << 31);
}

void lowerConstantSamplers(Module &M) {
	for (GlobalVariable &G : M.globals()) {
		if (!G.getName().starts_with("__air_sampler_state") || !G.hasInitializer())
			continue;
		if (auto *CI = dyn_cast<ConstantInt>(G.getInitializer()))
			G.setInitializer(ConstantInt::get(CI->getType(), samplerWord(CI->getZExtValue())));
	}
}

// air.sample_texture_2d.* (texture, sampler, coordinate, ...) -> {colour, i8}: rdna4_sample_2d of rdna4raster.c, level 0, offsets, bias
// and level of detail left out; air.sample_texture_1d_array.* (texture, sampler, coordinate, slice, ...): rdna4_sample_1d_array;
// air.sample_texture_3d.* (texture, sampler, coordinate, ...): rdna4_sample_3d; air.read_texture_2d.* of float texels (texture,
// coordinate, ...): rdna4_read_2d.
void lowerSampling(Module &M, Function &F) {
	LLVMContext &C = M.getContext();
	Type *f32 = Type::getFloatTy(C), *v4f = FixedVectorType::get(f32, 4);
	PointerType *tex = PointerType::get(C, kAmdGlobal), *smp = PointerType::get(C, kAmdConstant);
	auto sample2d = M.getOrInsertFunction("rdna4_sample_2d", FunctionType::get(v4f, {tex, smp, FixedVectorType::get(f32, 2)}, false));
	auto sample1da = M.getOrInsertFunction("rdna4_sample_1d_array", FunctionType::get(v4f, {tex, smp, f32, Type::getInt32Ty(C)}, false));
	auto read2d = M.getOrInsertFunction("rdna4_read_2d", FunctionType::get(v4f, {tex, Type::getInt32Ty(C), Type::getInt32Ty(C)}, false));
	auto sample3d = M.getOrInsertFunction("rdna4_sample_3d", FunctionType::get(v4f, {tex, smp, FixedVectorType::get(f32, 3)}, false));
	SmallVector<CallInst *, 8> calls;
	lowerConstantSamplers(M);
	for (Instruction &I : instructions(F))
		if (auto *CI = dyn_cast<CallInst>(&I))
			if (Function *callee = CI->getCalledFunction())
				if (callee->getName().starts_with("air.sample_texture_2d.") || callee->getName().starts_with("air.sample_texture_1d_array.") ||
				    callee->getName().starts_with("air.sample_texture_3d.") || callee->getName().starts_with("air.read_texture_2d."))
					calls.push_back(CI);
	for (CallInst *CI : calls) {
		auto *RT = dyn_cast<StructType>(CI->getType());
		if (!RT || RT->getNumElements() != 2 || components(RT->getElementType(0)) != 4)
			continue;
		IRBuilder<> B(CI);
		const StringRef name = CI->getCalledFunction()->getName();
		Value *v;
		if (name.starts_with("air.read_texture_2d.")) {
			// (texture, <2 x i32> coordinate, level, ...): float texels only, integer ones stay unsupported
			if (!RT->getElementType(0)->getScalarType()->isFloatingPointTy())
				continue;
			Value *xy = CI->getArgOperand(1);
			v = B.CreateCall(read2d, {CI->getArgOperand(0), B.CreateExtractElement(xy, uint64_t(0)), B.CreateExtractElement(xy, 1)});
		} else if (name.starts_with("air.sample_texture_3d.")) {
			v = B.CreateCall(sample3d, {CI->getArgOperand(0), CI->getArgOperand(1), CI->getArgOperand(2)});
		} else if (name.starts_with("air.sample_texture_1d_array.")) {
			v = B.CreateCall(sample1da, {CI->getArgOperand(0), CI->getArgOperand(1), CI->getArgOperand(2),
			                             B.CreateZExtOrTrunc(CI->getArgOperand(3), Type::getInt32Ty(C))});
		} else {
			v = B.CreateCall(sample2d, {CI->getArgOperand(0), CI->getArgOperand(1), CI->getArgOperand(2)});
		}
		if (RT->getElementType(0)->getScalarType()->isHalfTy())
			v = B.CreateFPTrunc(v, RT->getElementType(0));
		Value *r = B.CreateInsertValue(PoisonValue::get(RT), v, 0);
		r = B.CreateInsertValue(r, ConstantInt::get(RT->getElementType(1), 0), 1);
		CI->replaceAllUsesWith(r);
		CI->eraseFromParent();
	}
}

// The air.* calls of every function the module defines: the stage function's and those of the functions it calls (CoreAnimation's
// shaders keep some out of line), with the texture calls first for a render stage.
void lowerModule(Module &M, Stats &st, bool textures) {
	for (Function &G : M) {
		if (G.isDeclaration())
			continue;
		if (textures)
			lowerSampling(M, G);
		lowerFunction(G, st);
	}
}

// The AIR function inlined into its kernel, which takes its name.
Function *stageKernel(Module &M, Function *F, ArrayRef<Type *> params, const char *md) {
	LLVMContext &C = M.getContext();
	auto *NF = Function::Create(FunctionType::get(Type::getVoidTy(C), params, false), GlobalValue::ExternalLinkage, "", M);
	NF->setCallingConv(CallingConv::AMDGPU_KERNEL);
	const std::string name = F->getName().str();
	F->setName(name + ".air");
	F->setLinkage(GlobalValue::InternalLinkage);
	F->removeFnAttr(Attribute::NoInline);
	F->addFnAttr(Attribute::AlwaysInline);
	for (unsigned i = 0; i < F->arg_size(); i++)
		F->removeParamAttr(i, "air-buffer-no-alias");
	NF->setName(name);
	if (NamedMDNode *nm = M.getNamedMetadata(md))
		nm->eraseFromParent();
	// every function of the module inlined too, noinline ones included (CoreAnimation's apply_color_opcode_noinline): a call takes a
	// stack, which takes scratch memory the runtime does not set up
	for (Function &G : M) {
		if (G.isDeclaration() || &G == NF)
			continue;
		G.removeFnAttr(Attribute::NoInline);
		G.removeFnAttr(Attribute::OptimizeNone);
		G.addFnAttr(Attribute::AlwaysInline);
	}
	return NF;
}

const char *componentType(Type *T) {
	Type *e = T->getScalarType();
	return e->isHalfTy() ? "f16" : e->isFloatTy() ? "f32" : e->isIntegerTy(32) ? "i32" : e->isIntegerTy(16) ? "i16" : "?";
}

unsigned components(Type *T) {
	auto *VT = dyn_cast<FixedVectorType>(T);
	return VT ? VT->getNumElements() : 1;
}

// A vertex attribute of the pipeline's vertex descriptor, from --vertex-desc FILE (written by the compiler service), one line each:
// "attribute LOCATION FORMAT BUFFER OFFSET STRIDE STEP_FUNCTION STEP_RATE" (MTLVertexFormat, MTLVertexStepFunction).
struct VertexAttribute {
	unsigned format = 0, buffer = 0, offset = 0, stride = 0, step = 1, rate = 1;
};
using VertexDescriptor = std::map<unsigned, VertexAttribute>;

bool readVertexDesc(const std::string &path, VertexDescriptor &vd, std::string &err) {
	std::ifstream f(path);
	if (!f) {
		err = "cannot read " + path;
		return false;
	}
	std::string line;
	while (std::getline(f, line)) {
		std::istringstream ls(line);
		std::string what;
		unsigned loc;
		VertexAttribute a;
		if (ls >> what >> loc >> a.format >> a.buffer >> a.offset >> a.stride >> a.step >> a.rate && what == "attribute")
			vd[loc] = a;
	}
	return true;
}

// How a MTLVertexFormat stores its components.
enum class VertexEnc { U8, S8, UN8, SN8, U16, S16, UN16, SN16, F16, F32, I32, U32 };
struct VertexFormat {
	unsigned n;
	VertexEnc enc;
	bool bgra;
};

bool vertexFormat(unsigned f, VertexFormat &vf) {
	using E = VertexEnc;
	static const struct { unsigned first, last, n; E enc; } groups[] = {
		{1, 3, 2, E::U8}, {4, 6, 2, E::S8}, {7, 9, 2, E::UN8}, {10, 12, 2, E::SN8}, {13, 15, 2, E::U16}, {16, 18, 2, E::S16},
		{19, 21, 2, E::UN16}, {22, 24, 2, E::SN16}, {25, 27, 2, E::F16}, {28, 31, 1, E::F32}, {32, 35, 1, E::I32}, {36, 39, 1, E::U32},
		{45, 45, 1, E::U8}, {46, 46, 1, E::S8}, {47, 47, 1, E::UN8}, {48, 48, 1, E::SN8}, {49, 49, 1, E::U16}, {50, 50, 1, E::S16},
		{51, 51, 1, E::UN16}, {52, 52, 1, E::SN16}, {53, 53, 1, E::F16},
	};
	if (f == 42) {   // uchar4Normalized_BGRA
		vf = {4, E::UN8, true};
		return true;
	}
	for (const auto &g : groups)
		if (f >= g.first && f <= g.last) {
			vf = {g.n + f - g.first, g.enc, false};
			return true;
		}
	return false;
}

// One component as a float (or an i32 for the integer encodings), loaded at p.
Value *loadComponent(IRBuilder<> &B, Value *p, VertexEnc enc) {
	using E = VertexEnc;
	switch (enc) {
	case E::F32:
		return B.CreateAlignedLoad(B.getFloatTy(), p, Align(4));
	case E::F16:
		return B.CreateFPExt(B.CreateAlignedLoad(B.getHalfTy(), p, Align(2)), B.getFloatTy());
	case E::I32:
	case E::U32:
		return B.CreateAlignedLoad(B.getInt32Ty(), p, Align(4));
	default:
		break;
	}
	const bool wide = enc == E::U16 || enc == E::S16 || enc == E::UN16 || enc == E::SN16;
	const bool sign = enc == E::S8 || enc == E::SN8 || enc == E::S16 || enc == E::SN16;
	Type *T = wide ? B.getInt16Ty() : B.getInt8Ty();
	Value *v = B.CreateAlignedLoad(T, p, Align(wide ? 2 : 1));
	if (enc == E::U8 || enc == E::S8 || enc == E::U16 || enc == E::S16)
		return sign ? B.CreateSExt(v, B.getInt32Ty()) : B.CreateZExt(v, B.getInt32Ty());
	// normalized: unsigned to [0, 1], signed to [-1, 1]
	const float scale = wide ? (sign ? 32767.0f : 65535.0f) : (sign ? 127.0f : 255.0f);
	Value *f = sign ? B.CreateSIToFP(v, B.getFloatTy()) : B.CreateUIToFP(v, B.getFloatTy());
	f = B.CreateFMul(f, ConstantFP::get(B.getFloatTy(), 1.0f / scale));
	return sign ? B.CreateMaxNum(f, ConstantFP::get(B.getFloatTy(), -1.0f)) : f;
}

// A component as the argument's element type.
Value *convertComponent(IRBuilder<> &B, Value *v, Type *ET, VertexEnc enc) {
	const bool isInt = enc == VertexEnc::U8 || enc == VertexEnc::S8 || enc == VertexEnc::U16 || enc == VertexEnc::S16 ||
	                   enc == VertexEnc::I32 || enc == VertexEnc::U32;
	const bool sign = enc == VertexEnc::S8 || enc == VertexEnc::S16 || enc == VertexEnc::I32;
	if (ET->isFloatingPointTy()) {
		if (isInt)
			v = sign ? B.CreateSIToFP(v, B.getFloatTy()) : B.CreateUIToFP(v, B.getFloatTy());
		return B.CreateFPTrunc(v, ET);
	}
	if (!isInt)
		v = B.CreateFPToSI(v, B.getInt32Ty());
	return B.CreateZExtOrTrunc(v, ET);
}

// The attribute of element `index` at `base` as the argument's type T: the format's components, the missing ones (0, 0, 0, 1).
Value *fetchAttribute(IRBuilder<> &B, Value *base, Value *index, const VertexAttribute &a, const VertexFormat &vf, Type *T) {
	using E = VertexEnc;
	const unsigned size = vf.enc == E::F32 || vf.enc == E::I32 || vf.enc == E::U32 ? 4
	                      : vf.enc == E::U16 || vf.enc == E::S16 || vf.enc == E::UN16 || vf.enc == E::SN16 || vf.enc == E::F16 ? 2 : 1;
	Type *ET = T->getScalarType();
	Value *at = B.CreateGEP(B.getInt8Ty(), base, B.CreateAdd(B.CreateMul(B.CreateZExt(index, B.getInt64Ty()), B.getInt64(a.stride)),
	                                                         B.getInt64(a.offset)));
	Value *c[4];
	for (unsigned i = 0; i < 4; i++) {
		if (i < vf.n) {
			Value *v = loadComponent(B, B.CreateGEP(B.getInt8Ty(), at, B.getInt64(i * size)), vf.enc);
			c[i] = convertComponent(B, v, ET, vf.enc);
		} else {
			c[i] = ET->isFloatingPointTy() ? ConstantFP::get(ET, i == 3 ? 1.0 : 0.0) : ConstantInt::get(ET, i == 3);
		}
	}
	if (vf.bgra)
		std::swap(c[0], c[2]);
	const unsigned n = components(T);
	if (!T->isVectorTy())
		return c[0];
	Value *r = PoisonValue::get(T);
	for (unsigned i = 0; i < n && i < 4; i++)
		r = B.CreateInsertElement(r, c[i], i);
	return r;
}

// The vertex of thread `gid`: first + gid, or the index at gid of an indexed draw (type 1 uint16, 2 uint32), in blocks of its own.
Value *vertexId(IRBuilder<> &B, Function *NF, Value *gid, Value *first, Value *indices, Value *type) {
	LLVMContext &C = B.getContext();
	BasicBlock *from = B.GetInsertBlock();
	BasicBlock *i16b = BasicBlock::Create(C, "index16", NF), *i32b = BasicBlock::Create(C, "index32", NF);
	BasicBlock *join = BasicBlock::Create(C, "vertex", NF);
	Value *plain = B.CreateAdd(first, gid);
	SwitchInst *sw = B.CreateSwitch(type, join, 2);
	sw->addCase(B.getInt32(1), i16b);
	sw->addCase(B.getInt32(2), i32b);
	B.SetInsertPoint(i16b);
	Value *v16 = B.CreateZExt(B.CreateAlignedLoad(B.getInt16Ty(), B.CreateGEP(B.getInt16Ty(), indices, gid), Align(2)), B.getInt32Ty());
	B.CreateBr(join);
	B.SetInsertPoint(i32b);
	Value *v32 = B.CreateAlignedLoad(B.getInt32Ty(), B.CreateGEP(B.getInt32Ty(), indices, gid), Align(4));
	B.CreateBr(join);
	B.SetInsertPoint(join);
	PHINode *phi = B.CreatePHI(B.getInt32Ty(), 3);
	phi->addIncoming(plain, from);
	phi->addIncoming(v16, i16b);
	phi->addIncoming(v32, i32b);
	return phi;
}

// air2amdgcn vertex: rdna4_vertex(buffers..., u32 first, u32 count, u32 instance, ptr out, ptr indices, u32 index type): thread i runs the
// function for vertex first + i, or for indices[i] of an indexed draw (index type 1 uint16, 2 uint32, 0 none), and stores its result at
// out + i * stride. ABI: "buffer SLOT"..., "vertex" (the three u32), "out", "indices" (the pointer and the type), "stride BYTES", "position OFFSET",
// "output KEY OFFSET COMPONENTS TYPE" per output the fragment stage reads. Vertex inputs ([[stage_in]]) are loaded as the vertex
// descriptor says, from one more "buffer SLOT" per buffer they read (after the function's own).
bool lowerVertex(Module &M, const VertexDescriptor &vd, std::string &err, Stats &st) {
	LLVMContext &C = M.getContext();
	const DataLayout &DL = M.getDataLayout();
	std::vector<StageItem> args, outs;
	Function *F = stageEntry(M, "air.vertex", args, outs, err);
	if (!F)
		return false;
	lowerModule(M, st, false);
	if (!st.unsupported.empty()) {
		err = "unsupported air.* call: " + st.unsupported.begin()->first;
		return false;
	}
	Type *i32 = Type::getInt32Ty(C);
	std::vector<Type *> params;
	for (const StageItem &a : args)
		if (a.kind == "air.texture" || a.kind == "air.sampler") {
			err = "vertex function with textures or samplers: not lowered yet";
			return false;
		}
	resourceParams(C, args, params, st);
	std::map<unsigned, unsigned> fetchParam;   // buffer slot -> parameter
	for (const StageItem &a : args) {
		if (a.kind != "air.vertex_input")
			continue;
		auto it = vd.find(a.slot);
		VertexFormat vf;
		if (it == vd.end() || !vertexFormat(it->second.format, vf) || it->second.step > 2) {
			err = "vertex input '" + a.name + "' at attribute " + std::to_string(a.slot) + ": not in the vertex descriptor, or its format " +
			      "or step function has no lowering yet";
			return false;
		}
		if (fetchParam.count(it->second.buffer))
			continue;
		fetchParam[it->second.buffer] = params.size();
		params.push_back(PointerType::get(C, kAmdGlobal));
		st.abi.push_back("buffer " + std::to_string(it->second.buffer));
	}
	const size_t nBuf = params.size();
	params.insert(params.end(), {i32, i32, i32, PointerType::get(C, kAmdGlobal), PointerType::get(C, kAmdGlobal), i32});
	st.abi.push_back("vertex");
	st.abi.push_back("out");
	st.abi.push_back("indices");

	Type *RT = F->getReturnType();
	auto *STy = dyn_cast<StructType>(RT);
	if (RT->isVoidTy() || (!STy && outs.size() != 1) || (STy && STy->getNumElements() != outs.size())) {
		err = "vertex function outputs do not match its return type";
		return false;
	}
	const uint64_t stride = alignTo(DL.getTypeAllocSize(RT), 4);
	st.abi.push_back("stride " + std::to_string(stride));
	bool havePos = false;
	for (size_t i = 0; i < outs.size(); i++) {
		Type *ET = STy ? STy->getElementType(i) : RT;
		const uint64_t off = STy ? DL.getStructLayout(STy)->getElementOffset(i) : 0;
		if (outs[i].kind == "air.position") {
			st.abi.push_back("position " + std::to_string(off));
			havePos = true;
		} else if (outs[i].kind == "air.vertex_output") {
			st.abi.push_back("output " + outs[i].key + " " + std::to_string(off) + " " + std::to_string(components(ET)) + " " +
			                 componentType(ET));
		}
	}
	if (!havePos) {
		err = "vertex function without a position";
		return false;
	}

	Function *NF = stageKernel(M, F, params, "air.vertex");
	BasicBlock *entry = BasicBlock::Create(C, "entry", NF), *run = BasicBlock::Create(C, "run", NF), *done = BasicBlock::Create(C, "done", NF);
	IRBuilder<> B(entry);
	Value *gid = B.CreateAdd(B.CreateMul(B.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_x, ArrayRef<Value *>{}), B.getInt32(64)),
	                         B.CreateIntrinsic(Intrinsic::amdgcn_workitem_id_x, ArrayRef<Value *>{}));
	Argument *first = NF->getArg(nBuf), *count = NF->getArg(nBuf + 1), *instance = NF->getArg(nBuf + 2), *out = NF->getArg(nBuf + 3);
	Argument *indices = NF->getArg(nBuf + 4), *indexType = NF->getArg(nBuf + 5);
	B.CreateCondBr(B.CreateICmpULT(gid, count), run, done);
	B.SetInsertPoint(run);
	Value *vid = vertexId(B, NF, gid, first, indices, indexType);
	std::vector<Value *> callArgs;
	size_t bi = 0;
	for (size_t i = 0; i < args.size(); i++) {
		Type *T = F->getArg(i)->getType();
		const std::string &k = args[i].kind;
		if (k == "air.buffer") {
			callArgs.push_back(NF->getArg(bi++));
		} else if (k == "air.vertex_input") {
			const VertexAttribute &a = vd.at(args[i].slot);
			VertexFormat vf;
			vertexFormat(a.format, vf);
			Value *index = a.step == 1 ? vid : a.step == 2 && a.rate ? B.CreateUDiv(instance, B.getInt32(a.rate)) : B.getInt32(0);
			callArgs.push_back(fetchAttribute(B, NF->getArg(fetchParam.at(a.buffer)), index, a, vf, T));
		} else if (k == "air.vertex_id")
			callArgs.push_back(B.CreateZExtOrTrunc(vid, T));
		else if (k == "air.instance_id")
			callArgs.push_back(B.CreateZExtOrTrunc(instance, T));
		else if (k == "air.base_vertex")
			callArgs.push_back(B.CreateZExtOrTrunc(first, T));
		else if (k == "air.base_instance")
			callArgs.push_back(ConstantInt::get(T, 0));
		else {
			err = "vertex argument '" + args[i].name + "' of kind " + k + " has no lowering yet";
			NF->eraseFromParent();
			return false;
		}
	}
	Value *r = B.CreateCall(F, callArgs);
	Value *at = B.CreateGEP(Type::getInt8Ty(C), out, {B.CreateMul(B.CreateZExt(gid, B.getInt64Ty()), B.getInt64(stride))});
	B.CreateAlignedStore(r, at, Align(4));
	B.CreateBr(done);
	B.SetInsertPoint(done);
	B.CreateRetVoid();
	const unsigned tg[3] = {64, 1, 1};
	kernelAttributes(NF, tg);
	return true;
}

// What the vertex stage's ABI says about its records.
struct VertexRecord {
	unsigned stride = 0, position = 0;
	std::map<std::string, std::pair<unsigned, unsigned>> outputs;   // key -> byte offset, components (32-bit ones only)
};

bool readVertexAbi(const std::string &path, VertexRecord &v, std::string &err) {
	std::ifstream f(path);
	std::string line;
	bool stride = false;
	while (std::getline(f, line)) {
		std::istringstream is(line);
		std::string w, key, type;
		unsigned a = 0, b = 0;
		is >> w;
		if (w == "stride")
			stride = bool(is >> v.stride);
		else if (w == "position")
			is >> v.position;
		else if (w == "output" && (is >> key >> a >> b >> type)) {
			if (type == "f32" || type == "i32")
				v.outputs[key] = {a, b};
		}
	}
	if (!stride || v.stride % 4) {
		err = path + ": not a vertex ABI";
		return false;
	}
	return true;
}

// <4 x float> from rdna4raster as the AIR type of a fragment input: float or i32 scalars and vectors (the bits as stored), halves.
Value *fromFloat4(IRBuilder<> &B, Value *v, Type *T) {
	const unsigned n = components(T);
	Type *f32n = n == 1 ? B.getFloatTy() : (Type *)FixedVectorType::get(B.getFloatTy(), n);
	Value *f = n == 1 ? B.CreateExtractElement(v, uint64_t(0)) : B.CreateShuffleVector(v, ArrayRef<int>(std::vector<int>{0, 1, 2, 3}).take_front(n));
	Type *el = T->getScalarType();
	if (el->isFloatTy())
		return f;
	if (el->isHalfTy())
		return B.CreateFPTrunc(f, T);
	Type *i32n = n == 1 ? B.getInt32Ty() : (Type *)FixedVectorType::get(B.getInt32Ty(), n);
	Value *bits = B.CreateBitCast(f, i32n);
	(void)f32n;
	return B.CreateZExtOrTrunc(bits, T);
}

// air2amdgcn fragment: rdna4_fragment(buffers..., the draw: ptr verts, u32 stride, u32 position, u32 topology, u32 count, ptr target,
// u32 pitch, width, height, format, float viewport x, y, w, h, u32 blend, u32 scissor x, y, w, h), threadgroups of 8x8 pixels.
// ABI: "buffer SLOT"..., "fragment".
bool lowerFragment(Module &M, const VertexRecord &vr, std::string &err, Stats &st) {
	LLVMContext &C = M.getContext();
	std::vector<StageItem> args, outs;
	Function *F = stageEntry(M, "air.fragment", args, outs, err);
	if (!F)
		return false;
	lowerModule(M, st, true);
	if (!st.unsupported.empty()) {
		err = "unsupported air.* call: " + st.unsupported.begin()->first;
		return false;
	}
	// the colour of render target 0
	Type *RT = F->getReturnType();
	auto *STy = dyn_cast<StructType>(RT);
	int colour = -1;
	for (size_t i = 0; i < outs.size(); i++)
		if (outs[i].kind == "air.render_target" && colour < 0)
			colour = int(i);
	if (colour < 0 || (STy && unsigned(colour) >= STy->getNumElements()) || (!STy && colour != 0)) {
		err = "fragment function without a colour for render target 0";
		return false;
	}
	Type *CT = STy ? STy->getElementType(colour) : RT;
	if (!CT->getScalarType()->isFloatTy() && !CT->getScalarType()->isHalfTy()) {
		err = "fragment colour of a type other than float or half";
		return false;
	}

	Type *i32 = Type::getInt32Ty(C), *f32 = Type::getFloatTy(C), *gptr = PointerType::get(C, kAmdGlobal);
	std::vector<Type *> params;
	resourceParams(C, args, params, st);
	const size_t nBuf = params.size();
	params.insert(params.end(), {gptr, i32, i32, i32, i32, gptr, i32, i32, i32, i32, f32, f32, f32, f32, i32, i32, i32, i32, i32});
	st.abi.push_back("fragment");
	Function *NF = stageKernel(M, F, params, "air.fragment");

	// rdna4raster.c, linked in later
	Type *ptr = PointerType::get(C, 0), *v4f = FixedVectorType::get(f32, 4), *vd = Type::getVoidTy(C);
	auto decl = [&](const char *n, Type *ret, std::vector<Type *> ps) { return M.getOrInsertFunction(n, FunctionType::get(ret, ps, false)); };
	auto init = decl("rdna4_draw_init", vd, {ptr, gptr, i32, i32, i32, i32, gptr, i32, i32, i32, i32, f32, f32, f32, f32, i32, i32, i32, i32, i32});
	auto begin = decl("rdna4_px_begin", i32, {ptr, i32, i32, ptr});
	auto count = decl("rdna4_tri_count", i32, {ptr});
	auto cover = decl("rdna4_tri_cover", i32, {ptr, ptr, i32, ptr});
	auto interp = decl("rdna4_interp", v4f, {ptr, i32, i32, i32});
	auto fragPos = decl("rdna4_frag_position", v4f, {ptr, ptr});
	auto front = decl("rdna4_front_facing", i32, {ptr});
	auto blend = decl("rdna4_px_blend", vd, {ptr, ptr, v4f});
	auto end = decl("rdna4_px_end", vd, {ptr, ptr});

	BasicBlock *entry = BasicBlock::Create(C, "entry", NF), *head = BasicBlock::Create(C, "triangle", NF),
	           *test = BasicBlock::Create(C, "cover", NF), *shade = BasicBlock::Create(C, "shade", NF),
	           *write = BasicBlock::Create(C, "write", NF), *next = BasicBlock::Create(C, "next", NF),
	           *store = BasicBlock::Create(C, "store", NF);
	IRBuilder<> B(entry);
	auto block = [&](unsigned bytes, const char *name) {
		auto *a = B.CreateAlloca(ArrayType::get(B.getInt8Ty(), bytes), 5, nullptr, name);
		a->setAlignment(Align(16));
		return B.CreateAddrSpaceCast(a, ptr);
	};
	Value *draw = block(128, "draw"), *px = block(64, "px"), *tri = block(96, "tri");
	std::vector<Value *> initArgs{draw};
	for (size_t i = nBuf; i < NF->arg_size(); i++)
		initArgs.push_back(NF->getArg(i));
	B.CreateCall(init, initArgs);
	Value *x = B.CreateAdd(B.CreateMul(B.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_x, ArrayRef<Value *>{}), B.getInt32(8)),
	                       B.CreateIntrinsic(Intrinsic::amdgcn_workitem_id_x, ArrayRef<Value *>{}));
	Value *y = B.CreateAdd(B.CreateMul(B.CreateIntrinsic(Intrinsic::amdgcn_workgroup_id_y, ArrayRef<Value *>{}), B.getInt32(8)),
	                       B.CreateIntrinsic(Intrinsic::amdgcn_workitem_id_y, ArrayRef<Value *>{}));
	// Pixels outside the target or the scissor stay as helpers: every lane of a 2x2 quad runs the function when one of them is covered,
	// so that derivatives (dfdx) can read the others; only covered pixels blend.
	Value *inside = B.CreateICmpNE(B.CreateCall(begin, {draw, x, y, px}), B.getInt32(0));
	Value *n = B.CreateCall(count, {draw});
	Value *lane = B.CreateIntrinsic(i32, Intrinsic::amdgcn_mbcnt_lo, {B.getInt32(~0u), B.getInt32(0)});
	Value *quad = B.CreateShl(B.getInt32(0x303), B.CreateAnd(lane, B.getInt32(~9u)));
	B.CreateBr(head);

	B.SetInsertPoint(head);
	PHINode *t = B.CreatePHI(i32, 2, "t");
	t->addIncoming(B.getInt32(0), entry);
	B.CreateCondBr(B.CreateICmpULT(t, n), test, store);

	B.SetInsertPoint(test);
	Value *cov = B.CreateCall(cover, {draw, px, t, tri});
	Value *covered = B.CreateAnd(inside, B.CreateICmpEQ(cov, B.getInt32(1)));
	Value *quadCovered = B.CreateICmpNE(B.CreateAnd(B.CreateIntrinsic(i32, Intrinsic::amdgcn_ballot, {covered}), quad), B.getInt32(0));
	B.CreateCondBr(B.CreateAnd(quadCovered, B.CreateICmpNE(cov, B.getInt32(0))), shade, next);

	B.SetInsertPoint(shade);
	std::vector<Value *> callArgs;
	size_t bi = 0;
	for (size_t i = 0; i < args.size(); i++) {
		Type *T = F->getArg(i)->getType();
		const StageItem &a = args[i];
		if (isResource(a)) {
			callArgs.push_back(NF->getArg(bi++));
		} else if (a.kind == "air.position") {
			callArgs.push_back(fromFloat4(B, B.CreateCall(fragPos, {px, tri}), T));
		} else if (a.kind == "air.point_coord") {
			// only triangles are rasterized: a point's coordinate is the middle of the point
			callArgs.push_back(fromFloat4(B, ConstantVector::get({ConstantFP::get(f32, 0.5), ConstantFP::get(f32, 0.5), ConstantFP::get(f32, 0),
			                                                       ConstantFP::get(f32, 1)}), T));
		} else if (a.kind == "air.front_facing") {
			callArgs.push_back(B.CreateZExtOrTrunc(B.CreateICmpNE(B.CreateCall(front, {tri}), B.getInt32(0)), T));
		} else if (a.kind == "air.fragment_input") {
			auto o = vr.outputs.find(a.key);
			if (o == vr.outputs.end()) {
				err = "fragment input '" + a.name + "' (" + a.key + ") is no 32-bit output of the vertex function";
				NF->eraseFromParent();
				return false;
			}
			Value *v = B.CreateCall(interp, {tri, B.getInt32(o->second.first / 4), B.getInt32(components(T)), B.getInt32(a.interp)});
			callArgs.push_back(fromFloat4(B, v, T));
		} else {
			err = "fragment argument '" + a.name + "' of kind " + a.kind + " has no lowering yet";
			NF->eraseFromParent();
			return false;
		}
	}
	Value *r = B.CreateCall(F, callArgs);
	Value *c = STy ? B.CreateExtractValue(r, colour) : r;
	if (CT->getScalarType()->isHalfTy())
		c = B.CreateFPExt(c, FixedVectorType::get(f32, components(CT)));
	if (components(CT) != 4) {
		Value *v = ConstantVector::get({ConstantFP::get(f32, 0), ConstantFP::get(f32, 0), ConstantFP::get(f32, 0), ConstantFP::get(f32, 1)});
		for (unsigned k = 0; k < components(CT); k++)
			v = B.CreateInsertElement(v, components(CT) == 1 ? c : B.CreateExtractElement(c, k), k);
		c = v;
	}
	B.CreateCondBr(covered, write, next);

	B.SetInsertPoint(write);
	B.CreateCall(blend, {draw, px, c});
	B.CreateBr(next);

	B.SetInsertPoint(next);
	Value *t1 = B.CreateAdd(t, B.getInt32(1));
	t->addIncoming(t1, next);
	B.CreateBr(head);

	B.SetInsertPoint(store);
	B.CreateCall(end, {draw, px});
	B.CreateRetVoid();

	const unsigned tg[3] = {8, 8, 1};
	kernelAttributes(NF, tg);
	(void)vr.position;
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

// AIR "thread" memory is alloca in address space 0; the AMDGPU data layout puts allocas in the private space (5). Re-create each alloca there and cast
// the result back to a flat pointer, the way clang does for OpenCL/HIP; the back end's address-space inference removes most casts.
void fixAllocas(Module &M) {
	for (Function &F : M) {
		SmallVector<AllocaInst *, 8> as;
		for (Instruction &I : instructions(F))
			if (auto *A = dyn_cast<AllocaInst>(&I))
				if (A->getAddressSpace() != 5)
					as.push_back(A);
		for (AllocaInst *A : as) {
			// lifetime markers must name the alloca itself, not a cast of it: drop them (they only inform optimisation)
			SmallVector<Instruction *, 4> markers;
			for (User *U : A->users())
				if (auto *II = dyn_cast<IntrinsicInst>(U))
					if (II->isLifetimeStartOrEnd())
						markers.push_back(II);
			for (Instruction *m : markers)
				m->eraseFromParent();
			auto *N = new AllocaInst(A->getAllocatedType(), 5, A->getArraySize(), A->getAlign(), A->getName(), A->getIterator());
			auto *C = new AddrSpaceCastInst(N, A->getType(), "", A->getIterator());
			A->replaceAllUsesWith(C);
			A->eraseFromParent();
		}
	}
}

void retarget(Module &M, TargetMachine &TM) {
	M.setTargetTriple(Triple("amdgcn-amd-amdhsa"));
	M.setDataLayout(TM.createDataLayout());
	fixAllocas(M);
	// threadgroup (LDS) globals: AMDGPU accepts only undef/poison initializers; Metal threadgroup memory is uninitialised anyway
	for (GlobalVariable &G : M.globals())
		if (G.getAddressSpace() == kAmdLocal && G.hasInitializer() && !isa<UndefValue>(G.getInitializer()))
			G.setInitializer(PoisonValue::get(G.getValueType()));
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
		fprintf(stderr, "usage: %s kernel IN --tg X,Y,Z -o OUT.ll [--entry NAME] [--mcpu gfx1201] [--abi OUT.abi]\n"
		                "       %s vertex IN -o OUT.ll --abi OUT.abi [--vertex-desc DESC]\n"
		                "       %s fragment IN --vertex-abi VERTEX.abi -o OUT.ll --abi OUT.abi\n"
		                "       %s coverage [--codegen] IN...\n", argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	const std::string mode = argv[1];
	std::string out, entry, cpu = "gfx1201", tgs = "64,1,1", abi, vertexAbi, vertexDesc;
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
		else if (a == "--abi" && i + 1 < argc)
			abi = argv[++i];
		else if (a == "--vertex-abi" && i + 1 < argc)
			vertexAbi = argv[++i];
		else if (a == "--vertex-desc" && i + 1 < argc)
			vertexDesc = argv[++i];
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
		if (!abi.empty()) {
			raw_fd_ostream as(abi, ec);
			for (const std::string &l : st.abi)
				as << l << "\n";
		}
		fprintf(stderr, "air2amdgcn: lowered");
		for (auto &l : st.lowered)
			fprintf(stderr, " %s x%u", l.first.c_str(), l.second);
		fprintf(stderr, "%s\n", st.lowered.empty() ? " (no air.* calls)" : "");
		return 0;
	}

	if (mode == "vertex" || mode == "fragment") {
		if (inputs.size() != 1 || out.empty() || abi.empty() || (mode == "fragment" && vertexAbi.empty())) {
			fprintf(stderr, "%s mode: one input, -o, --abi%s\n", mode.c_str(), mode == "fragment" ? " and --vertex-abi" : "");
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
		VertexRecord vr;
		VertexDescriptor vd;
		M = remapConstantSpace(std::move(M), ctx, err);
		bool ok = M != nullptr && (vertexDesc.empty() || readVertexDesc(vertexDesc, vd, err));
		if (ok) {
			M->setDataLayout(TM->createDataLayout());   // the vertex records are laid out as the AMDGPU back end stores them
			ok = mode == "vertex" ? lowerVertex(*M, vd, err, st) : readVertexAbi(vertexAbi, vr, err) && lowerFragment(*M, vr, err, st);
		}
		if (!ok) {
			fprintf(stderr, "air2amdgcn: %s: %s\n", inputs[0].c_str(), err.c_str());
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
		raw_fd_ostream as(abi, ec);
		for (const std::string &l : st.abi)
			as << l << "\n";
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
						F.removeFnAttr("frame-pointer");   // callable C-convention functions: amdgpu_gfx/amdgpu_ps are refused under the HSA OS
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
