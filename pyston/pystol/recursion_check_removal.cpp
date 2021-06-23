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

bool RemoveRecursionChecksPass::isThreadStatePointerLoad(Value* v) {
    auto load = dyn_cast<LoadInst>(v);
    if (!load)
        return false;
    if (load->getOrdering() != AtomicOrdering::Monotonic)
        return false;

    auto gep = dyn_cast<GEPOperator>(load->getPointerOperand());
    if (!gep)
        return false;
    if (!gep->hasAllConstantIndices() || gep->getNumIndices() != 4)
        return false;

    int i = 0;
    for (auto &idx : iterator_range<User::op_iterator>(gep->idx_begin(), gep->idx_end())) {
        if (i == 0 && cast<ConstantInt>(idx)->getSExtValue() != 0)
            return false;
        if (i == 1 && cast<ConstantInt>(idx)->getSExtValue() != 12)
            return false;
        if (i == 2 && cast<ConstantInt>(idx)->getSExtValue() != 1)
            return false;
        if (i == 3 && cast<ConstantInt>(idx)->getSExtValue() != 0)
            return false;
        if (i >= 4)
            return false;
        i++;
    }

    auto gv = dyn_cast<GlobalVariable>(gep->getPointerOperand());
    if (!gv)
        return false;

    if (!isNamedStructPointer(gv->getType(), "struct.pyruntimestate"))
        return false;
    if (gv->getName() != "_PyRuntime")
        return false;

    return true;
}

bool RemoveRecursionChecksPass::isRecursionEnter(Instruction* inst, RecursionEnterCheck& output) {
    auto store = dyn_cast<StoreInst>(inst);
    if (!store)
        return false;

    if (!isRecursionDepthIncrement(store->getValueOperand(), 1, output))
        return false;

    if (!isRecursionDepthPointer(store->getPointerOperand(), output))
        return false;

    auto br = dyn_cast<BranchInst>(inst->getParent()->getTerminator());
    if (!br->isConditional())
        return false;

    output.overflow_br = br;

    auto exc_block = br->getSuccessor(0);
    auto call = dyn_cast<CallInst>(exc_block->getFirstNonPHIOrDbgOrLifetime());
    if (!call)
        return false;

    auto called_func = call->getCalledFunction();
    if (!called_func || called_func->getName() != "_Py_CheckRecursiveCall")
        return false;

    output.exc_block = exc_block;
    output.incdepth = store;

    return true;
}

bool RemoveRecursionChecksPass::isRecursionLeave(Instruction* inst, RecursionLeaveCheck& output) {
    auto store = dyn_cast<StoreInst>(inst);
    if (!store)
        return false;

    if (!isRecursionDepthIncrement(store->getValueOperand(), -1, output))
        return false;

    if (!isRecursionDepthPointer(store->getPointerOperand(), output))
        return false;

    output.decdepth = store;

    auto leave_value = store->getValueOperand();

    auto br = cast<BranchInst>(store->getParent()->getTerminator());
    if (!br->isConditional()) {
        // call.c:function_code_fastcall directly manipulates tstate->recursion_depth
        // This case isn't removable, but maybe there are other similar ones that are?
        return false;
    }

    auto underflow_block = br->getSuccessor(0);
    auto load = dyn_cast<LoadInst>(underflow_block->getFirstNonPHIOrDbgOrLifetime());
    if (!load)
        return false;
    if (load->getOrdering() != AtomicOrdering::Monotonic)
        return false;
    RELEASE_ASSERT(
        cast<GlobalVariable>(cast<GEPOperator>(load->getPointerOperand())
                                 ->getPointerOperand())->getName()
            == "_PyRuntime",
        "");

    output.underflow_block = underflow_block;
    output.underflow_br = br;

    return true;
}

static StringSet<> guard_unnecessary_functions;
bool RemoveRecursionChecksPass::needsRecursionGuard(Instruction* inst) {
    auto call = dyn_cast<CallBase>(inst);
    if (!call)
        return false;

    auto func = call->getCalledFunction();
    if (!func)
        return true;

    auto name = func->getName();

    if (name.startswith("llvm.lifetime.start"))
        return false;

    if (guard_unnecessary_functions.count(name))
        return false;

    /* Things that are trickier:
     * unicode_new can call unicode_subtype_new which calls tp_alloc
     * range_new calls PyNumber_Index which calls nb_index
     * _Py_Dealloc calls tp_dealloc
     * PyObject_IsTrue calls nb_bool / others
     * PyObject_Not calls PyObject_IsTrue
     * object_richcompare calls self->ob_type->tp_richcompare.  I think it's recursive, but I think with a tighter idea of when we can eliminate checks we could probably ignore these calls
     */
    if (name != "unicode_new" && name != "range_new"
        && name != "_Py_Dealloc" && name != "PyObject_IsTrue"
        && name != "PyObject_Not" && name != "object_richcompare")
        outs() << "Not sure if this function needs recursion guard: " << name << "\n";
    return true;
}

Value* RemoveRecursionChecksPass::getSingleUse(Value* v) {
    Value* r = nullptr;
    for (auto &u : v->uses()) {
        RELEASE_ASSERT(!r, "");
        r = u.getUser();
    }
    return r;
}

// Given an Instruction with zero uses, remove that instruction,
// and remove any instructions that now have zero uses without side effects
void RemoveRecursionChecksPass::removeUnusedInsts(Instruction* inst) {
    RELEASE_ASSERT(getSingleUse(inst) == nullptr,
                   "Trying to remove a used value!");

    SmallVector<Value*, 4> ops(inst->operands());
    inst->eraseFromParent();

    for (auto op : ops) {
        auto op_inst = dyn_cast<Instruction>(op);
        if (!op_inst)
            continue;
        if (!op_inst->use_empty())
            continue;

        if (isa<ICmpInst>(op_inst) || isa<BinaryOperator>(op_inst)
            || isa<SelectInst>(op_inst) || isa<GetElementPtrInst>(op_inst)
            || isa<PHINode>(op_inst) || isa<CastInst>(op_inst)) {
            removeUnusedInsts(op_inst);
        } else if (auto load = dyn_cast<LoadInst>(op_inst)) {
            if (load->getOrdering() == AtomicOrdering::NotAtomic || isThreadStatePointerLoad(load))
                removeUnusedInsts(op_inst);
        } else {
            outs() << "Not sure if we can remove this unused inst: " << *op_inst << '\n';
        }
    }
}

void RemoveRecursionChecksPass::removeCheck(const RecursionEnterCheck& enter,
                 const RecursionLeaveCheck& leave) {
    RELEASE_ASSERT(enter.overflow_br->getSuccessor(0) == enter.exc_block, "");
    auto new_branch = BranchInst::Create(enter.overflow_br->getSuccessor(1),
                                         enter.overflow_br->getParent());
    auto enter_br_cond = enter.overflow_br->getCondition();
    enter.overflow_br->eraseFromParent();

    auto leave_value = leave.decdepth->getValueOperand();

    enter.incdepth->eraseFromParent();
    leave.decdepth->eraseFromParent();

    Value* cmp = getSingleUse(leave_value);

    RELEASE_ASSERT(leave.underflow_br->getCondition() == cmp, "");
    RELEASE_ASSERT(leave.underflow_br->getSuccessor(0) == leave.underflow_block, "");
    new_branch = BranchInst::Create(leave.underflow_br->getSuccessor(1), leave.underflow_br->getParent());
    leave.underflow_br->eraseFromParent();

    removeUnusedInsts(cast<Instruction>(enter_br_cond));
    removeUnusedInsts(cast<Instruction>(cmp));
}

bool RemoveRecursionChecksPass::maybeRemoveCheck(const RecursionEnterCheck& enter,
                      const RecursionLeaveCheck& leave) {
    vector<Instruction*> between(BlockMatcher::instructionsBetween(enter.incdepth, leave.decdepth, enter.exc_block));
    for (auto inst : between) {
        if (needsRecursionGuard(inst)) {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                outs() << "This instruction needs recursion guarding, not removing:\n";
                outs() << *inst << '\n';
            }
            return false;
        }
    }
    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
        outs() << "can remove this recursion check!\n";

    removeCheck(enter, leave);
    return true;
}

RemoveRecursionChecksPass::~RemoveRecursionChecksPass() {
    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
        outs() << "Removed " << num_removed << " unnecessary recursion check pairs\n";
}

bool RemoveRecursionChecksPass::runOnFunction(Function &F) {
    // TODO: maybe this should be a different FunctionPass
    // Remove unused thread state pointer loads.
    // Since they're atomic, llvm won't eliminate them even
    // if they have no uses.  I don't think we need to keep them around
    // though.
    for (auto &BB : F) {
        SmallVector<Instruction*, 4> to_remove;
        for (auto &I : BB) {
            if (I.use_empty() && isThreadStatePointerLoad(&I)) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)  {
                    outs() << "Removing unused ThreadState load\n";
                    outs() << I << '\n';
                }
                to_remove.push_back(&I);
            }
        }
        for (auto inst : to_remove)
            inst->eraseFromParent();
    }

    // TODO this should be a different function pass
    for (auto &BB : F) {
        for (auto &I : BB) {
            if (auto call = dyn_cast<CallInst>(&I)) {
                auto func = call->getCalledFunction();
                if (func && func->getName() == "PyErr_NoMemory")
                    call->replaceAllUsesWith(ConstantInt::getNullValue(call->getType()));
            }
        }
    }

    SmallVector<RecursionEnterCheck, 4> enters;
    for (auto &BB : F) {
        for (auto &I : BB) {
            RecursionEnterCheck check;
            if (isRecursionEnter(&I, check)) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                    outs() << "Found recursion enter check:\n";
                    outs() << I << '\n';
                }
                enters.push_back(check);
            }
        }
    }

    SmallVector<RecursionLeaveCheck, 4> leaves;
    for (auto &BB : F) {
        for (auto &I : BB) {
            RecursionLeaveCheck check;
            if (isRecursionLeave(&I, check)) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                    outs() << "Found recursion leave check:\n";
                    outs() << I << '\n';
                }
                leaves.push_back(check);
            }
        }
    }

    BlockMatcher matcher(&F);

    SmallVector<bool, 4> enter_matched(enters.size(), false);
    SmallVector<bool, 4> leave_matched(leaves.size(), false);

    for (int i = 0; i < enters.size(); i++) {
        if (enter_matched[i])
            continue;
        auto& enter = enters[i];
        for (int j = 0; j < leaves.size(); j++) {
            if (leave_matched[j])
                continue;
            auto& leave = leaves[j];

            // We ignore the enter.exc_block, since going that path can skip the LeaveRecursiveCall call.
            // The exc_block will take care of reducing the recursion depth
            if (matcher.blocksAreMatched(enter.incdepth->getParent(), leave.decdepth->getParent(), enter.exc_block)) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                    outs() << "found a matched recursion enter+leave!\n";
                    outs() << *enter.incdepth << '\n';
                    outs() << *leave.decdepth << '\n';
                }

                if (maybeRemoveCheck(enter, leave)) {
                    enter_matched[i] = leave_matched[j] = true;
                    num_removed++;
                    break;
                }
            }
        }
    }

    return false;
}

char RemoveRecursionChecksPass::ID = 0;

StringSet<> RemoveRecursionChecksPass::guard_unnecessary_functions({
    "llvm.assume",
    "llvm.dbg.value",
    "llvm.dbg.label",
    "llvm.fabs.f64",
    "frexp",
    "modf",
    "bcmp",
    "wmemcmp",
    "memcmp",

    // No recursive behavior:
    "PyType_IsSubtype",
    "_PyArg_CheckPositional",
    "PyArg_UnpackTuple",
    "long_compare",
    "long_richcompare",
    "PyLong_FromDouble",
    "PyUnicode_RichCompare",
    "range_richcompare",
    "PyBool_FromLong",
    "PyErr_Format", // Little bit less sure about this one
    "float_richcompare",
    "_PyUnicode_Ready", // Little bit less sure about this one
    "unicode_compare",
    "set_richcompare",
    "Py_FatalError",
    "_Py_CheckFunctionResult",
    "_PyErr_FormatFromCause", // Little bit less sure about this one
    "PyObject_Malloc",
    "_PyTraceMalloc_NewReference",

    // Do their own recursion checking:
    "PyObject_RichCompare",
    "PyObject_RichCompareBool",
    "PyObject_Str",
    "PyObject_Repr",
    "set_lookkey",
    "set_issubset",
    "slice_richcompare",
    "list_richcompare",
    "tuplerichcompare",
    "dict_richcompare",
});

} // namespace pystol
