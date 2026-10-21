// air2amdgpu: one compute kernel of Apple AIR (the LLVM bitcode Metal compiles to, as metallibs carry it) as an AMDGPU object for gfx1201
// (M3 of docs/metal-phase-plan.md, track b: AIR -> LLVM AMDGPU). Upstream LLVM reads AIR as it is (air64_v28, Metal 4, tested with LLVM 23):
// buffers are `ptr addrspace(1)` as on AMDGPU, and what makes a function a kernel is only metadata (!air.kernel, one record per argument).
//
//   air2amdgpu <kernel.air or .bc> <kernel name> <out.o>      then: ld.lld -shared out.o -o out.hsaco
//
// It prints `<name> <buffers> <write mask>` on stdout (bit i: buffer index i is written), for tools/metal/m3/build.sh's table.
//
// The object holds an amdgpu_kernel of the same name whose kernargs are (the ABI the fake paravirtual GPU's host fills, src/pvgpu.cpp):
//   +0           a 64-bit buffer address per Metal buffer index, 0 .. the highest index the kernel uses
//   +8 * N       u32 threads per threadgroup x, y, z
//   +8 * N + 12  u32 threads in the grid x, y, z (threads past it do nothing: dispatchThreads need not fill whole groups)
// It computes the AIR function's position arguments from the work-group and work-item ids and calls the AIR function, inlined.
// Supported so far: device buffers (address space 1) and the arguments thread_position_in_grid, thread_position_in_threadgroup,
// threadgroup_position_in_grid, threads_per_threadgroup and threads_per_grid, as uint, uint2 or uint3. Anything else is reported and refused.
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include <map>
#include <string>

using namespace llvm;

static int fail(const std::string &why) {
	errs() << "air2amdgpu: " << why << "\n";
	return 1;
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

int main(int argc, char **argv) {
	if (argc != 4)
		return fail("usage: air2amdgpu <kernel.air> <kernel name> <out.o>");
	LLVMContext ctx;
	SMDiagnostic diag;
	std::unique_ptr<Module> m = parseIRFile(argv[1], diag, ctx);
	if (!m)
		return fail("cannot read " + std::string(argv[1]) + ": " + diag.getMessage().str());
	Function *air = m->getFunction(argv[2]);
	NamedMDNode *kernels = m->getNamedMetadata("air.kernel");
	const MDNode *args = nullptr;
	for (unsigned i = 0; kernels && i < kernels->getNumOperands(); i++) {
		const MDNode *k = kernels->getOperand(i);
		if (k->getNumOperands() >= 3 && mdconst::dyn_extract<Function>(k->getOperand(0)) == air)
			args = dyn_cast<MDNode>(k->getOperand(2));
	}
	if (!air || !args)
		return fail(std::string("no AIR kernel named ") + argv[2]);

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
			if (location < 0 || location > 31 || index >= air->arg_size() || air->getArg(index)->getType() != PointerType::get(ctx, 1))
				return fail("argument " + std::to_string(index) + ": only device buffers (address space 1) are supported so far");
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
			return fail("argument " + std::to_string(index) + ": " + kind + " is not supported yet");
		}
	}
	if (roles.size() != air->arg_size())
		return fail("not every argument of the AIR function is described");

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
	Value *group[3], *item[3], *size[3], *grid[3], *pos[3];
	const Intrinsic::ID groupId[3] = {Intrinsic::amdgcn_workgroup_id_x, Intrinsic::amdgcn_workgroup_id_y, Intrinsic::amdgcn_workgroup_id_z};
	const Intrinsic::ID itemId[3] = {Intrinsic::amdgcn_workitem_id_x, Intrinsic::amdgcn_workitem_id_y, Intrinsic::amdgcn_workitem_id_z};
	Value *inside = b.getTrue();
	for (unsigned d = 0; d < 3; d++) {
		group[d] = b.CreateIntrinsic(groupId[d], {});
		item[d] = b.CreateIntrinsic(itemId[d], {});
		size[d] = kernel->getArg(nBuffers + d);
		grid[d] = kernel->getArg(nBuffers + 3 + d);
		pos[d] = b.CreateAdd(b.CreateMul(group[d], size[d]), item[d]);
		inside = b.CreateAnd(inside, b.CreateICmpULT(pos[d], grid[d]));
	}
	BasicBlock *run = BasicBlock::Create(ctx, "run", kernel), *done = BasicBlock::Create(ctx, "done", kernel);
	b.CreateCondBr(inside, run, done);
	b.SetInsertPoint(run);
	SmallVector<Value *, 16> callArgs;
	for (unsigned i = 0; i < air->arg_size(); i++) {
		const Arg &a = roles[i];
		Value *v = a.role == Buffer      ? kernel->getArg(static_cast<unsigned>(a.location))
		         : a.role == PosInGrid   ? asType(b, air->getArg(i)->getType(), pos)
		         : a.role == PosInGroup  ? asType(b, air->getArg(i)->getType(), item)
		         : a.role == GroupInGrid ? asType(b, air->getArg(i)->getType(), group)
		         : a.role == GroupSize   ? asType(b, air->getArg(i)->getType(), size)
		                                 : asType(b, air->getArg(i)->getType(), grid);
		if (!v)
			return fail("argument " + std::to_string(i) + ": an unsupported type for a position");
		callArgs.push_back(v);
	}
	b.CreateCall(air, callArgs);
	b.CreateBr(done);
	b.SetInsertPoint(done);
	b.CreateRetVoid();

	// Retarget: AMDGPU's triple and data layout, no AIR metadata.
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmPrinters();
	const Triple triple("amdgcn-amd-amdhsa");
	std::string err;
	const Target *target = TargetRegistry::lookupTarget(triple, err);
	if (!target)
		return fail("no AMDGPU target in this LLVM: " + err);
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
		return fail("the retargeted module does not verify");

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
			return fail("private memory (an alloca in AIR's address space 0) is not supported yet");

	std::error_code ec;
	raw_fd_ostream out(argv[3], ec, sys::fs::OF_None);
	if (ec)
		return fail("cannot write " + std::string(argv[3]) + ": " + ec.message());
	legacy::PassManager cg;
	if (tm->addPassesToEmitFile(cg, out, nullptr, CodeGenFileType::ObjectFile))
		return fail("this LLVM cannot emit an AMDGPU object");
	cg.run(*m);
	out.flush();
	outs() << name << " " << nBuffers << " " << written << "\n";
	return 0;
}
