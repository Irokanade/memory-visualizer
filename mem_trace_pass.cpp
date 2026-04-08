#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace llvm;

struct MemTracePass : PassInfoMixin<MemTracePass> {
    static bool isRequired() { return true; }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &)
    {
        Module *M = F.getParent();
        LLVMContext &Ctx = M->getContext();
        const DataLayout &DL = M->getDataLayout();

        FunctionType *FnTy = FunctionType::get(
            Type::getVoidTy(Ctx),
            {Type::getInt64Ty(Ctx), Type::getInt32Ty(Ctx)}, false);

        FunctionCallee ReadFn = M->getOrInsertFunction("cachesim_read", FnTy);
        FunctionCallee WriteFn = M->getOrInsertFunction("cachesim_write", FnTy);

        SmallVector<Instruction *> to_instrument;
        for (auto &BB : F) {
            for (auto &I : BB) {
                if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
                    to_instrument.push_back(&I);
                }
            }
        }

        for (auto *I : to_instrument) {
            IRBuilder<> B(I);

            Value *ptr;
            uint64_t size;
            FunctionCallee fn;

            if (auto *LI = dyn_cast<LoadInst>(I)) {
                ptr = LI->getPointerOperand();
                size = DL.getTypeStoreSize(LI->getType());
                fn = ReadFn;
            } else {
                auto *SI = cast<StoreInst>(I);
                ptr = SI->getPointerOperand();
                size = DL.getTypeStoreSize(SI->getValueOperand()->getType());
                fn = WriteFn;
            }

            Value *addr = B.CreatePtrToInt(ptr, Type::getInt64Ty(Ctx));
            Value *sz = ConstantInt::get(Type::getInt32Ty(Ctx), size);
            B.CreateCall(fn, {addr, sz});
        }

        if (to_instrument.empty()) {
            return PreservedAnalyses::all();
        }
        return PreservedAnalyses::none();
    }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return {LLVM_PLUGIN_API_VERSION, "MemTrace", "v0.1", [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "mem-trace") {
                            FPM.addPass(MemTracePass());
                            return true;
                        }
                        return false;
                    });
                PB.registerOptimizerLastEPCallback([](ModulePassManager &MPM,
                                                      OptimizationLevel,
                                                      ThinOrFullLTOPhase) {
                    FunctionPassManager FPM;
                    FPM.addPass(MemTracePass());
                    MPM.addPass(
                        createModuleToFunctionPassAdaptor(std::move(FPM)));
                });
            }};
}
