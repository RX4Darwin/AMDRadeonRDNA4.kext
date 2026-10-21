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
// threadgroup_position_in_grid, threads_per_threadgroup and threads_per_grid (uint, uint2 or uint3), threadgroup memory and barriers, local
// arrays (as long as LLVM keeps them out of scratch memory, which the kext's runtime does not give kernels), device and threadgroup atomic
// read-modify-writes, and the math and integer functions of legalize() below. Anything else is reported and refused.
#include "lld/Common/Driver.h"
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
		if (name == "wg.barrier") {
			// (memory flags: 1 device, 2 threadgroup, 4 texture; scope): fence, the hardware barrier, fence.
			const SyncScope::ID scope = ctx.getOrInsertSyncScopeID(cast<ConstantInt>(c->getArgOperand(0))->getZExtValue() & 1 ? "agent" : "workgroup");
			b.CreateFence(AtomicOrdering::Release, scope);
			b.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {});
			b.CreateFence(AtomicOrdering::Acquire, scope);
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

static bool gLldSpent;   // lld said it cannot be run again in this process: --serve exits after its reply (launchd starts a new one)

struct Kernel {
	std::string hsaco;   // the linked code object
	unsigned buffers;
	uint32_t written;
};

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
	enum Role { Buffer, PosInGrid, PosInGroup, GroupInGrid, GroupSize, GridSize };
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
		                                 : asType(b, air->getArg(i)->getType(), grid);
		if (!v)
			return "argument " + std::to_string(i) + ": an unsupported type for a position";
		callArgs.push_back(v);
	}
	b.CreateCall(air, callArgs);
	b.CreateBr(done);
	b.SetInsertPoint(done);
	b.CreateRetVoid();

	// The AIR function and whatever it calls inlined into the kernel; the module's other kernels go (they need not translate).
	for (unsigned i = 0; kernels && i < kernels->getNumOperands(); i++)
		if (auto *k = mdconst::dyn_extract_or_null<Function>(kernels->getOperand(i)->getOperand(0)); k && k != air) {
			k->replaceAllUsesWith(PoisonValue::get(k->getType()));
			k->eraseFromParent();
		}
	for (bool again = true; again;) {
		again = false;
		for (Instruction &i : instructions(*kernel))
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
	if (std::string why = legalize(*kernel); !why.empty())
		return why;

	// Retarget: AMDGPU's triple and data layout, no AIR metadata.
	const Triple triple("amdgcn-amd-amdhsa");
	std::string err;
	const Target *target = TargetRegistry::lookupTarget(triple, err);
	if (!target)
		return "no AMDGPU target in this LLVM: " + err;
	std::unique_ptr<TargetMachine> tm(target->createTargetMachine(triple, "gfx1201", "", TargetOptions(), std::nullopt));
	m->setTargetTriple(triple);
	m->setDataLayout(tm->createDataLayout());
	SmallVector<NamedMDNode *, 8> airMd;
	for (NamedMDNode &n : m->named_metadata())
		if (n.getName().starts_with("air."))
			airMd.push_back(&n);
	for (NamedMDNode *n : airMd)
		m->eraseNamedMetadata(n);
	if (verifyModule(*m, &errs()))
		return "the retargeted module does not verify";

	// Only the new kernel stays visible: the module's other AIR kernels and functions go unless it uses them.
	for (Function &f : *m)
		if (&f != kernel && !f.isDeclaration())
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
	pb.buildPerModuleDefaultPipeline(OptimizationLevel::O2).run(*m, mam);
	for (Instruction &i : instructions(*kernel))
		if (isa<AllocaInst>(i))
			return "private memory (an alloca in AIR's address space 0) is not supported yet";

	// Emit the object and link it into a code object (ld.lld -shared), through temporary files: lld's driver reads and writes files.
	SmallVector<char, 0> object;
	raw_svector_ostream objectOut(object);
	legacy::PassManager cg;
	if (tm->addPassesToEmitFile(cg, objectOut, nullptr, CodeGenFileType::ObjectFile))
		return "this LLVM cannot emit an AMDGPU object";
	cg.run(*m);
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
