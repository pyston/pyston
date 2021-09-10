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
using namespace nitrous;

namespace pystol {

class MiscOptsPass : public FunctionPass {
public:
    static char ID;

    LLVMEvaluator& eval;

    MiscOptsPass(LLVMEvaluator& eval) : llvm::FunctionPass(ID), eval(eval) {}

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

        // remove refcount changes to immortal objects
        // looking for:
        //  store i64 %57, i64* getelementptr inbounds (%struct._typeobject, %struct._typeobject* @PyLong_Type, i64 0, i32 0, i32 0, i32 0)
        for (auto &BB : F) {
            for (auto it = BB.begin(); it != BB.end();) {
                auto& DL = F.getParent()->getDataLayout();

                auto store = dyn_cast<StoreInst>(&*it++);
                if (!store)
                    continue;
                // ob_refcnt is 64bit
                if (!store->getValueOperand()->getType()->isIntegerTy(64))
                    continue;
                auto gep = dyn_cast<GEPOperator>(store->getPointerOperand());
                if (!gep)
                    continue;

                // is the destination type a PyObject or PyTypeObject?
                auto ptr = gep->getPointerOperand();
                if (!isPyObjectPtr(ptr->getType()) && !isPyTypeObjectPtr(ptr->getType()))
                    continue;

                // check if gep is accessing PyObject->ob_refcnt
                if (!gep->hasAllConstantIndices())
                    continue;
                APInt offset(8 * sizeof(void*), 0);
                bool success = gep->accumulateConstantOffset(DL, offset);
                if (!success || offsetof(PyObject, ob_refcnt) != offset)
                    continue;

                auto C = dyn_cast<Constant>(ptr);
                if (!C)
                    continue;
                auto obj = (PyObject*)GVTOP(eval.evalConstant(C));
                if (!obj)
                    continue;
                auto&& is_immortal = [](PyObject* obj) {
                    return obj->ob_refcnt > (1L<<59);
                };
                if (!is_immortal(obj))
                    continue;

                if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS) {
                    outs() << "removing refcount change to immortal object:\n";
                    outs() << "\t" << *store << "\n";
                }

                changed = true;
                it = store->eraseFromParent();
            }
        }

        return changed;
    }
};
char MiscOptsPass::ID;

FunctionPass* createMiscOptsPass(LLVMEvaluator& eval) {
    return new MiscOptsPass(eval);
}

// Pass which tracks whether an exception could have been thrown and
// eliminates unnecessary exception checks.
// This comes up if we are able to eliminate enough other code that
// we know no exceptions could have been thrown.
//
// Todo: this currently assumes that all functions start with no exception
// set. This is an assumption that should be provided by aot_gen rather than
// hard-coded here
class ExceptionTrackingPass : public FunctionPass {
private:
    bool canCauseException(Instruction *I) {
        if (auto store = dyn_cast<StoreInst>(I)) {
            if (store->getValueOperand()->getType()->isIntegerTy())
                return false;
            return true;
        }

        if (auto call = dyn_cast<CallInst>(I)) {
            auto func = call->getCalledFunction();

            if (!func)
                return true;

            auto name = func->getName();

            return name != "llvm.assume" && name != "llvm.dbg.value" && name != "llvm.returnaddress" && name != "PyErr_Occurred";
        }

        return false;
    }

public:
    static char ID;

    ExceptionTrackingPass() : llvm::FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
        // Whether an exception could be thrown by the end of this block:
        DenseMap<BasicBlock*, bool> can_throw;

        for (auto &BB : F) {
            can_throw[&BB] = false;
            for (auto &I : BB) {
                if (canCauseException(&I)) {
                    can_throw[&BB] = true;
                    break;
                }
            }
        }

        vector<BasicBlock*> to_visit;
        for (auto &BB : F) {
            if (can_throw[&BB])
                to_visit.push_back(&BB);
        }

        while (!to_visit.empty()) {
            auto *BB = to_visit.back();
            to_visit.pop_back();
            for (auto *BB2 : successors(BB)) {
                if (!can_throw[BB2]) {
                    can_throw[BB2] = true;
                    to_visit.push_back(BB2);
                }
            }
        }

        bool changed = false;
        for (auto &BB : F) {
            // Simplification: rather than track per-instruction can-throw-ness, just use
            // the fact that if it can't throw by the end of the block it couldn't have
            // thrown by any of the instructions
            if (can_throw[&BB])
                continue;

            for (auto &I : BB) {
                if (auto call = dyn_cast<CallInst>(&I)) {
                    auto func = call->getCalledFunction();
                    if (func && func->getName() == "PyErr_Occurred") {
                        call->replaceAllUsesWith(ConstantInt::getNullValue(call->getType()));
                        call->eraseFromParent();
                        changed = true;
                        break;
                    }
                }
            }
        }

        return changed;
    }
};
char ExceptionTrackingPass::ID;

FunctionPass* createExceptionTrackingPass(LLVMEvaluator& eval) {
    return new ExceptionTrackingPass();
}

} // namespace pystol
