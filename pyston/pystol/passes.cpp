#include <stddef.h>
#include <vector>

#include "llvm/ADT/StringSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#define Py_BUILD_CORE

#include "Python.h"
#include "frameobject.h"
#include "internal/pycore_pystate.h"

#include "common.h"
#include "optimization_hooks.h"

#include "pystol.h"
#include "pystol_internal.h"

using namespace llvm;
using namespace std;

namespace pystol {

class MiscOptsPass : public FunctionPass {
public:
    static char ID;

    MiscOptsPass() : llvm::FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
        bool changed = false;

        // Optimization: PyObject_GetItemLong must be called with argument 2 as the unboxed value of argument 1
        // Todo: add this knowledge as a __builtin_assume call rather than hard-coding it off of the function name
        if (F.getName().startswith("PyObject_GetItemLong")) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    if (auto call = dyn_cast<CallInst>(&I)) {
                        auto func = call->getCalledFunction();
                        if (func && func->getName() == "PyLong_AsSsize_t") {
                            if (call->getOperand(0)->stripPointerCastsAndAliases() == F.getArg(1)) {
                                call->replaceAllUsesWith(F.getArg(2));
                                changed = true;
                                call->eraseFromParent();
                                break;
                            }
                        }
                    }
                }
            }
        }

        return changed;
    }
};
char MiscOptsPass::ID;

FunctionPass* createMiscOptsPass() {
    return new MiscOptsPass();
}

class ExceptionTrackingPass : public FunctionPass {
public:
    static char ID;

    ExceptionTrackingPass() : llvm::FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
        return false;
    }
};
char ExceptionTrackingPass::ID;

FunctionPass* createExceptionTrackingPass() {
    return new ExceptionTrackingPass();
}

} // namespace pystol
