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

#define LOCFOR(type, name) Location(offsetof(type, name), sizeof(type::name))

#define MARKCONST(ptr, size) addJitConst((char*)(ptr), (size), JIT_IS_CONST)
#define MARKNOTZERO(ptr, size) addJitConst((char*)(ptr), (size), JIT_NOT_ZERO)

using namespace llvm;
using namespace nitrous;
using namespace std;

namespace pystol {

void addImmortalTuple(PyObject* o) {
    RELEASE_ASSERT(o->ob_type == &PyTuple_Type, "");

    long size = Py_SIZE(o);

    char* end = (char*)&PyTuple_GET_ITEM(o, size);

    // ob_type is first field after ob_refcnt
    RELEASE_ASSERT(offsetof(PyObject, ob_refcnt) == 0, "");
    RELEASE_ASSERT(offsetof(PyObject, ob_type) == 8, "");
    int offset = offsetof(PyObject, ob_type);
    char* start = ((char*)o) + offset;
    MARKNOTZERO((char*)o, offset); /* refcount is never zero but not const! */
    MARKCONST(start, end - start);
}

// Copied and modified from CPython's type___subclasses___impl:
static vector<PyTypeObject*> getImmediateSubclasses(PyTypeObject* self) {
    PyObject* raw, *ref;
    Py_ssize_t i;

    vector<PyTypeObject*> list;
    raw = self->tp_subclasses;
    if (raw == NULL)
        return list;
    assert(PyDict_CheckExact(raw));
    i = 0;
    while (PyDict_Next(raw, &i, NULL, &ref)) {
        assert(PyWeakref_CheckRef(ref));
        ref = PyWeakref_GET_OBJECT(ref);
        if (ref != Py_None) {
            list.push_back((PyTypeObject*)ref);
        }
    }
    return list;
}

static void addConstTypeAndConstSubclasses(PyTypeObject* type) {
    //printf("Saying %s is const\n", type->tp_name);
    pystolAddConstType(type);

    for (PyTypeObject* subtype : getImmediateSubclasses(type)) {
        if (!(subtype->tp_flags & Py_TPFLAGS_HEAPTYPE))
            addConstTypeAndConstSubclasses(subtype);
    }
}

static bool cantChangeClass(PyTypeObject* type) {
    // This logic comes from typeobject.c:object_set_class
    return !PyType_IsSubtype(type, &PyModule_Type) && !(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
}

class PystolFactDeriver : public FactDeriver {
public:
    bool deriveFacts(Value* v, FactSet& facts, LLVMEvaluator& eval) override;
};

static bool isNamedStructPointer(Type* t, const char* name) {
    auto p = dyn_cast<PointerType>(t);
    if (!p)
        return false;
    auto st = dyn_cast<StructType>(p->getElementType());
    if (!st)
        return false;
    return st->getName() == name;
}

static bool isPyObjectPtr(Type* t) {
    return isNamedStructPointer(t, "struct._object");
}

static Knowledge* getTypeFact(Value* v, FactSet& facts) {
    auto it = facts.facts.find(LOCFOR(PyObject, ob_type));
    if (it == facts.facts.end())
        return NULL;
    return &it->second;
}

extern "C" {
PyObject* _PyEval_EvalFrame_AOT(PyFrameObject*, int);
}

bool PystolFactDeriver::deriveFacts(Value* v, FactSet& facts, LLVMEvaluator& eval) {
    bool changed = false;

    if (auto fact = getTypeFact(v, facts)) {
        if (fact->known_value && fact->known_at) {
            if (auto gv = dyn_cast<GlobalVariable>(fact->known_value)) {
                PyTypeObject* type = (PyTypeObject*)GVTOP(eval.evalConstant(gv));
                if (cantChangeClass(type)) {
                    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                        outs() << "Promoting this fact since the type " << type->tp_name << " can't change:\n";
                        fact->dump();
                        outs() << '\n';
                    }
                    fact->known_at = NULL;
                    changed = true;
                    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                        fact->dump();
                        outs() << '\n';
                    }
                }
            }
        }

        if (fact->known_value && !fact->known_at) {
            if (auto gv = dyn_cast<GlobalVariable>(fact->known_value)) {
                PyTypeObject* type = (PyTypeObject*)GVTOP(eval.evalConstant(gv));
                if (type == &PyFunction_Type) {
                    // functions have fixed "vectorcall" members
                    // In C, the code to access it is
                    //   ptr = (vectorcallfunc*)(((char *)callable) + offset);
                    // After we const-load offset, llvm translates this to
                    //   *(vectorcallfunc*)&((PyObject*)callable)[7]
                    // So we have to put the fact at gep 7
                    Knowledge& k = facts[LOCFOR(PyFunctionObject, vectorcall)];
                    if (!k.known_value || k.known_at) {
                        // Hack, we just need a pointer type
                        Type* t = v->getType()->getPointerTo();
                        k.known_value = eval.GVToConst(GenericValue((void*)_PyFunction_Vectorcall), t);
                        k.known_at = NULL;
                        changed = true;
                        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                            outs() << "Know that " << *v << " is a Function object, so the vectorcall offset has value " << *k.known_value << '\n';
                    }
                }
            }
        }
    }

    if (isNamedStructPointer(v->getType(), "struct._is")) {
        // _is::eval_frame
        typedef struct _is _is;
        Knowledge& k = facts[LOCFOR(_is, eval_frame)];
        // We fix eval_frame to _PyEval_EvalFrame_AOT since we know we are running with that to get to this trace.
        // This means that it becomes more difficult to change the frame evaluation function a second time,
        // but I think it's ok if we don't support that.
        if (!k.known_value || k.known_at) {
            // Hack, we just need a pointer type
            Type* t = v->getType()->getPointerTo();
            k.known_value = eval.GVToConst(GenericValue((void*)_PyEval_EvalFrame_AOT), t);
            k.known_at = NULL;
            changed = true;
        }
    }

    if (auto load = dyn_cast<LoadInst>(v)) {
        if (auto gv = dyn_cast<GlobalVariable>(load->getPointerOperand())) {
            auto name = gv->getName();
            if (name == "float_free_list" || name == "list_free_list"
                || name == "frame_free_list" || name == "dict_free_list" || name == "free_list") {
                auto&& knowledge = facts[Location()];
                if (!knowledge.isheapalloc) {
                    knowledge.isheapalloc = true;
                    changed = true;
                }
            }
        }
    }

    auto updateKnowledgeHeapalloc = [&](Knowledge& knowledge) {
        if (!knowledge.isheapalloc) {
            knowledge.isheapalloc = true;
            changed = true;
        }
    };

    auto updateKnowledgeValue = [&](Knowledge& knowledge, Value* known_value, Instruction* known_at=nullptr) {
        if (!knowledge.known_value || (knowledge.known_at && !known_at)) {
            knowledge.known_value = known_value;
            knowledge.known_at = known_at;
            changed = true;
        }
    };

    if (auto call = dyn_cast<CallInst>(v)) {
        if (auto func = dyn_cast<Function>(call->getCalledOperand())) {
            auto&& name = func->getName();
            if (name == "PyObject_Malloc") {
                // PyObject_Malloc is an allocation function:
                updateKnowledgeHeapalloc(facts[Location()]);
            } else if (name == "PyErr_NoMemory") {
                // PyErr_NoMemory returns a null pointer:
                updateKnowledgeHeapalloc(facts[Location()]);
                updateKnowledgeValue(facts[Location()], ConstantInt::getNullValue(call->getType()));
            } else if (name == "_PyFrame_New_NoTrack") {
                // _PyFrame_New_NoTrack always returns a frame object:
                updateKnowledgeValue(facts[LOCFOR(PyObject, ob_type)],
                    eval.GVToConst(GenericValue((void*)&PyFrame_Type), call->getType()));
            } else if (name == "PyTuple_New" || name == "PyTuple_New_Nonzeroed") {
                RELEASE_ASSERT(isPyObjectPtr(call->getType()), "");

                Type* t = call->getType();
                t = call->getParent()->getParent()->getParent()->getTypeByName("struct._typeobject")->getPointerTo();

                if (auto cint = dyn_cast<ConstantInt>(call->getOperand(0))) {
                    long val = cint->getSExtValue();
                    // PyTuple_New(0) returns a reference to a shared empty tuple
                    if (val != 0) {
                        updateKnowledgeValue(facts[LOCFOR(PyObject, ob_refcnt)],
                            ConstantInt::get(Type::getInt64Ty(getContext()), 1),
                            call);
                    }
                }
                updateKnowledgeValue(facts[LOCFOR(PyObject, ob_type)],
                    eval.GVToConst(GenericValue((void*)&PyTuple_Type), call->getType()));
                updateKnowledgeValue(facts[LOCFOR(PyTupleObject, ob_base.ob_size)], call->getOperand(0));
                updateKnowledgeHeapalloc(facts[Location()]);
            }
        }
    }

    if (auto fact = getTypeFact(v, facts)) {
        if (fact->known_value && !fact->known_at) {
            if (auto gv = dyn_cast<GlobalVariable>(fact->known_value)) {
                PyTypeObject* type = (PyTypeObject*)GVTOP(eval.evalConstant(gv));

                if (type == &PyTuple_Type && isPyObjectPtr(v->getType())) {
                    for (auto&& p : facts.facts) {
                        // Tuple ob_item are locations starting at 1,1
                        if (p.first.indirections.size() != 1)
                            continue;
                        int offset = p.first.indirections[0].offset;
                        if (offset < offsetof(PyTupleObject, ob_item))
                            continue;

                        if (p.second.known_value && p.second.known_at) {
                            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                                outs() << "Tuple " << *v << " has known fact:\n";
                                p.first.dump();
                                outs() << ' ';
                                p.second.dump();
                                outs() << "\nwhich we are promoting to always known\n";
                            }
                            p.second.known_at = NULL;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    return changed;
}

static string getBBName(BasicBlock* bb) {
    SmallVector<char, 4> sv;
    raw_svector_ostream os(sv);
    os << *bb;
    StringRef s = os.str();
    size_t idx = s.find(':');
    if (idx == StringRef::npos)
        return "entry";
    return s.substr(1, idx-1).str();
}

class BlockMatcher {
    /* BlockMatcher:
     * Computes whether two basic blocks are "matched".
     * Blocks A and B are matched if the execution occurrences
     * of A and B match the pattern (AB)*
     * ie they alternate, with A starting first, and ending with B
     * Note that this relationship is not symmetric.
     *
     * If there are blocks ending with an "unreachable" instruction,
     * the pattern is allowed to be (AB)*AX where X is the block ending
     * with unreachable.
     *
     * In terms of implementation, the "matched" relationship
     * is broken down into the intersection of two propreties.
     * In the string of A and B occurrences:
     * - B can only come after an A (not after a B or at the beginning)
     * - After each A must be a B (not an A or the end)
     *
     * Together, these show that the string must match (AB)*
     * They are the same property about A and B but reversed
     *
     * Concretely, "B can only come after an A" is equivalent to
     * "B and the entry block have no paths to B that don't go
     * through A".
     * Similarly, "After each A must be a B" is equivalent to
     * "A has no paths to A or an exit block that doesn't go through B"
     */
private:
    Function* func;
    typedef SmallVector<BasicBlock*, 4> BBVector;

    BBVector exit_blocks;

    // Returns whether there is a path from any block in `from` to any
    // block in `to` without going through `without`.
    // The path is considered to be coming out of `from` and needs to go in
    // to `to`.  In particular this means that if there is a shared element
    // in the two vectors, it doesn't automatically count as a path.
    bool canReach(const BBVector& from, const BBVector& to, const BBVector& without) {
        DenseSet<BasicBlock*> without_set;
        for (auto BB : without)
            without_set.insert(BB);

        DenseSet<BasicBlock*> to_set;
        for (auto BB : to)
            to_set.insert(BB);

        DenseSet<BasicBlock*> reached;
        vector<BasicBlock*> queue;

        for (auto BB : from) {
            if (without_set.count(BB))
                continue;

            queue.push_back(BB);
        }

        while (!queue.empty()) {
            BasicBlock* bb = queue.back();
            queue.pop_back();
            for (auto BB : successors(bb)) {
                if (without_set.count(BB))
                    continue;
                if (reached.count(BB))
                    continue;
                if (to_set.count(BB))
                    return true;

                queue.push_back(BB);
                reached.insert(BB);
            }
        }

        return false;
    }

public:
    BlockMatcher(Function* func) : func(func) {
        for (auto &BB : *func) {
            if (succ_empty(&BB) && !isa<UnreachableInst>(BB.getTerminator()))
                exit_blocks.push_back(&BB);
        }
    }

    // `avoid` consists of blocks that are to be ignored from the CFG.
    // It should only be used in specific circumstances
    bool blocksAreMatched(BasicBlock* A, BasicBlock* B,
                          BasicBlock* avoid = nullptr) {
        BBVector a_v({A});
        BBVector b_v({B});
        BBVector b_and_entry({B, &func->getEntryBlock()});

        BBVector a_and_exits(exit_blocks);
        a_and_exits.push_back(A);

        BBVector a_avoid({A});
        BBVector b_avoid({B});
        if (avoid) {
            a_avoid.push_back(avoid);
            b_avoid.push_back(avoid);
        }

        bool x = canReach(b_and_entry, b_v, a_avoid);
        bool y = canReach(a_v, a_and_exits, b_avoid);
        return !x && !y;
    }

    // Returns the list of instructions that could be executed between an instruction A and instruction B.
    // Makes the most sense if A->getParent() and B->getParent() are matched blocks.
    static vector<Instruction*>
    instructionsBetween(Instruction* A, Instruction* B,
                        BasicBlock* avoid = nullptr) {
        vector<Instruction*> r;

        bool seen = false;
        for (auto &I : *A->getParent()) {
            if (seen)
                r.push_back(&I);
            if (&I == A)
                seen = true;
        }

        for (auto &I : *B->getParent()) {
            if (&I == B)
                break;
            r.push_back(&I);
        }


        DenseSet<BasicBlock*> visited({ A->getParent(), B->getParent(), avoid });
        vector<BasicBlock*> queue;

        auto pushSuccessors = [&](BasicBlock* bb) {
            for (auto *BB : successors(bb)) {
                if (visited.count(BB))
                    continue;
                visited.insert(BB);
                queue.push_back(BB);
            }
        };

        pushSuccessors(A->getParent());

        while (!queue.empty()) {
            auto* bb = queue.back();
            queue.pop_back();
            pushSuccessors(bb);
            for (auto &I : *bb) {
                r.push_back(&I);
            }
        }

        return r;
    }
};

// A pass to identify and remove unnecessary recursion checks.
// Recursion checks are a bit tricky to identify, since they consist
// of ~10 instructions on either side.  Right now we try to match
// the specific instruction pattern that clang emits.
// Here's an example of what the recursion check pair looks like:
/*
define %struct._object* @cmp_outcomePyCmp_EQFloatFloat2(%struct._object* nocapture readonly %0, %struct._object* nocapture readonly %1, %struct.AOTFunc* nocapture readnone %2) local_unnamed_addr #0 {
  %4 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %5 = inttoptr i64 %4 to %struct._ts*
  %6 = getelementptr inbounds %struct._ts, %struct._ts* %5, i64 0, i32 4
  %7 = load i32, i32* %6, align 8, !tbaa !4
  %8 = add i32 %7, 1
  store i32 %8, i32* %6, align 8, !tbaa !4
  %9 = load i32, i32* @_Py_CheckRecursionLimit, align 4, !tbaa !12
  %10 = icmp sgt i32 %8, %9
  br i1 %10, label %11, label %15, !prof !13

11:                                               ; preds = %3
  %12 = tail call i32 @_Py_CheckRecursiveCall(i8* getelementptr inbounds ([15 x i8], [15 x i8]* @.str.25.4573, i64 0, i64 0)) #3
  %13 = icmp eq i32 %12, 0
  br i1 %13, label %14, label %47

14:                                               ; preds = %11
  %.pre = load i32, i32* @_Py_CheckRecursionLimit, align 4, !tbaa !12
  br label %15

15:                                               ; preds = %14, %3
  %16 = phi i32 [ %.pre, %14 ], [ %9, %3 ]
  %17 = getelementptr inbounds %struct._object, %struct._object* %0, i64 1
  %18 = bitcast %struct._object* %17 to double*
  %19 = load double, double* %18, align 8, !tbaa !14
  %20 = getelementptr inbounds %struct._object, %struct._object* %1, i64 1
  %21 = bitcast %struct._object* %20 to double*
  %22 = load double, double* %21, align 8, !tbaa !14
  %23 = fcmp une double %19, %22
  %24 = select i1 %23, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0, i32 0), i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0, i32 0)
  %25 = select i1 %23, %struct._object* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0), %struct._object* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0)
  %26 = load i64, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0, i32 0), align 8
  %27 = icmp ne i64 %26, 0
  tail call void @llvm.assume(i1 %27)
  %28 = load i64, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0, i32 0), align 8
  %29 = icmp ne i64 %28, 0
  tail call void @llvm.assume(i1 %29)
  %30 = select i1 %23, i64 %26, i64 %28
  %31 = add i64 %30, 1
  store i64 %31, i64* %24, align 8, !tbaa !18
  %32 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %33 = inttoptr i64 %32 to %struct._ts*
  %34 = getelementptr inbounds %struct._ts, %struct._ts* %33, i64 0, i32 4
  %35 = load i32, i32* %34, align 8, !tbaa !4
  %36 = add i32 %35, -1
  store i32 %36, i32* %34, align 8, !tbaa !4
  %37 = icmp sgt i32 %16, 200
  %38 = add i32 %16, -50
  %39 = ashr i32 %16, 2
  %40 = mul nsw i32 %39, 3
  %41 = select i1 %37, i32 %38, i32 %40
  %42 = icmp slt i32 %36, %41
  br i1 %42, label %43, label %47, !prof !19

43:                                               ; preds = %15
  %44 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %45 = inttoptr i64 %44 to %struct._ts*
  %46 = getelementptr inbounds %struct._ts, %struct._ts* %45, i64 0, i32 5
  store i8 0, i8* %46, align 4, !tbaa !20
  br label %47

47:                                               ; preds = %43, %15, %11
  %48 = phi %struct._object* [ null, %11 ], [ %25, %43 ], [ %25, %15 ]
  ret %struct._object* %48
}
*/
class RemoveRecursionChecksPass : public FunctionPass {
private:
    int num_removed = 0;

    struct RecursionEnterCheck {
        StoreInst* incdepth;
        BasicBlock* exc_block;
        BranchInst* overflow_br;
    };

    struct RecursionLeaveCheck {
        StoreInst* decdepth;
        BasicBlock* underflow_block;
        BranchInst* underflow_br;
    };

    bool isThreadStatePointerLoad(Value* v) {
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

    template <typename T>
    bool isThreadStatePointer(Value* v, T& output) {
        auto cast = dyn_cast<IntToPtrInst>(v);
        if (!cast)
            return false;

        if (!isNamedStructPointer(cast->getType(), "struct._ts"))
            return false;

        return isThreadStatePointerLoad(cast->getOperand(0));
    }

    template <typename T>
    bool isRecursionDepthPointer(Value* v, T& output) {
        auto gep = dyn_cast<GetElementPtrInst>(v);
        if (!gep)
            return false;

        if (!gep->hasAllConstantIndices() || gep->getNumIndices() != 2)
            return false;

        int i = 0;
        for (auto &idx : gep->indices()) {
            if (i == 0 && cast<ConstantInt>(idx)->getSExtValue() != 0)
                return false;
            if (i == 1 && cast<ConstantInt>(idx)->getSExtValue() != 4)
                return false;
            i++;
        }
        return isThreadStatePointer(gep->getPointerOperand(), output);
    }

    template <typename T>
    bool isRecursionDepthLoad(Value* v, T& output) {
        auto load = dyn_cast<LoadInst>(v);
        if (!load)
            return false;
        return isRecursionDepthPointer(load->getPointerOperand(), output);
    }

    template <typename T>
    bool isRecursionDepthIncrement(Value* v, int amount, T& output) {
        auto add = dyn_cast<BinaryOperator>(v);
        if (!add)
            return false;
        if (add->getOpcode() != Instruction::Add)
            return false;

        auto op1 = dyn_cast<ConstantInt>(add->getOperand(1));
        if (!op1)
            return false;
        if (op1->getSExtValue() != amount)
            return false;
        return isRecursionDepthLoad(add->getOperand(0), output);
    }

    bool isRecursionEnter(Instruction* inst, RecursionEnterCheck& output) {
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


    bool isRecursionLeave(Instruction* inst, RecursionLeaveCheck& output) {
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
    bool needsRecursionGuard(Instruction* inst) {
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

    Value* getSingleUse(Value* v) {
        Value* r = nullptr;
        for (auto &u : v->uses()) {
            RELEASE_ASSERT(!r, "");
            r = u.getUser();
        }
        return r;
    }

    // Given an Instruction with zero uses, remove that instruction,
    // and remove any instructions that now have zero uses without side effects
    void removeUnusedInsts(Instruction* inst) {
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

    void removeCheck(const RecursionEnterCheck& enter,
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

    bool maybeRemoveCheck(const RecursionEnterCheck& enter,
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

public:
    static char ID;

    RemoveRecursionChecksPass() : FunctionPass(ID) {}
    ~RemoveRecursionChecksPass() {
        if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
            outs() << "Removed " << num_removed << " unnecessary recursion check pairs\n";
    }

    bool runOnFunction(Function &F) override {
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
};
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
using namespace pystol;

extern "C" {

void pystolGlobalPythonSetup() {
    addMallocLikeFunc("PyObject_Malloc");
    addMallocLikeFunc("PyTuple_New");
    addMallocLikeFunc("PyTuple_New_Nonzeroed");

    pystolAddConstObj(Py_True);
    pystolAddConstObj(Py_False);
    pystolAddConstObj(Py_None);
    pystolAddConstObj(Py_NotImplemented);

    MARKCONST((char*)_Py_SwappedOp, sizeof(_Py_SwappedOp[0]) * 6);

    addConstTypeAndConstSubclasses(&PyBaseObject_Type);

    registerFactDeriver(make_unique<PystolFactDeriver>());
    registerPassFactory([] { return new RemoveRecursionChecksPass(); });
}

void pystolAddConstObj(PyObject* x) {
    int offset = offsetof(PyObject, ob_type); /* skip refcount */
    MARKNOTZERO((char*)x, offset); /* refcount is never zero but not const! */
    if (PyLong_CheckExact(x))
        MARKCONST(((char*)x) + offset, sizeof(PyLongObject)
                                           + (Py_SIZE(x) - 1) * sizeof(digit)
                                           - offset);
    else if (PyFloat_CheckExact(x))
        MARKCONST(((char*)x) + offset, sizeof(PyFloatObject) - offset);
    else
        MARKCONST(((char*)x) + offset, sizeof(PyObject) - offset);
}

void pystolAddConstType(PyTypeObject* x) {
    // refcount is never zero but not const!
    MARKNOTZERO(&x->ob_base.ob_base.ob_refcnt, sizeof(x->ob_base.ob_base.ob_refcnt));

    MARKCONST(&x->tp_name, sizeof(x->tp_name));
    MARKCONST(&x->tp_basicsize, sizeof(x->tp_basicsize));
    MARKCONST(&x->tp_itemsize, sizeof(x->tp_itemsize));
    MARKCONST(&x->tp_dealloc, sizeof(x->tp_dealloc));
    MARKCONST(&x->tp_vectorcall_offset, sizeof(x->tp_vectorcall_offset));
    MARKCONST(&x->tp_getattr, sizeof(x->tp_getattr));
    MARKCONST(&x->tp_setattr, sizeof(x->tp_setattr));
    MARKCONST(&x->tp_as_async, sizeof(x->tp_as_async));
    if (x->tp_as_async)
        MARKCONST(x->tp_as_async, sizeof(*x->tp_as_async));
    MARKCONST(&x->tp_repr, sizeof(x->tp_repr));
    MARKCONST(&x->tp_as_number, sizeof(x->tp_as_number));
    if (x->tp_as_number)
        MARKCONST(x->tp_as_number, sizeof(*x->tp_as_number));
    MARKCONST(&x->tp_as_sequence, sizeof(x->tp_as_sequence));
    if (x->tp_as_sequence)
        MARKCONST(x->tp_as_sequence, sizeof(*x->tp_as_sequence));
    MARKCONST(&x->tp_as_mapping, sizeof(x->tp_as_mapping));
    if (x->tp_as_mapping)
        MARKCONST(x->tp_as_mapping, sizeof(*x->tp_as_mapping));
    MARKCONST(&x->tp_hash, sizeof(x->tp_hash));
    MARKCONST(&x->tp_call, sizeof(x->tp_call));
    MARKCONST(&x->tp_str, sizeof(x->tp_str));
    MARKCONST(&x->tp_getattro, sizeof(x->tp_getattro));
    MARKCONST(&x->tp_setattro, sizeof(x->tp_setattro));
    MARKCONST(&x->tp_as_buffer, sizeof(x->tp_as_buffer));
    if (x->tp_as_buffer)
        MARKCONST(x->tp_as_buffer, sizeof(*x->tp_as_buffer));
    MARKCONST(&x->tp_flags, sizeof(x->tp_flags)); // not sure if this should be const
    MARKCONST(&x->tp_doc, sizeof(x->tp_doc));
    MARKCONST(&x->tp_traverse, sizeof(x->tp_traverse));
    MARKCONST(&x->tp_clear, sizeof(x->tp_clear));
    MARKCONST(&x->tp_richcompare, sizeof(x->tp_richcompare));
    MARKCONST(&x->tp_weaklistoffset, sizeof(x->tp_weaklistoffset));
    MARKCONST(&x->tp_iter, sizeof(x->tp_iter));
    MARKCONST(&x->tp_iternext, sizeof(x->tp_iternext));
    MARKCONST(&x->tp_methods, sizeof(x->tp_methods));
    MARKCONST(&x->tp_members, sizeof(x->tp_members));
    MARKCONST(&x->tp_getset, sizeof(x->tp_getset));
    MARKCONST(&x->tp_base, sizeof(x->tp_base)); // not sure if this should be const

    // not const in AOT mode:
    // PyObject *tp_dict;

    MARKCONST(&x->tp_descr_get, sizeof(x->tp_descr_get));
    MARKCONST(&x->tp_descr_set, sizeof(x->tp_descr_set));
    MARKCONST(&x->tp_dictoffset, sizeof(x->tp_dictoffset));
    MARKCONST(&x->tp_init, sizeof(x->tp_init));
    MARKCONST(&x->tp_alloc, sizeof(x->tp_alloc));
    MARKCONST(&x->tp_new, sizeof(x->tp_new));
    MARKCONST(&x->tp_free, sizeof(x->tp_free));
    MARKCONST(&x->tp_is_gc, sizeof(x->tp_is_gc));

    // not const in AOT mode:
    // PyObject *tp_bases;
    // PyObject *tp_mro;
    // PyObject *tp_cache;
    // PyObject *tp_subclasses;
    // PyObject *tp_weaklist;

    MARKCONST(&x->tp_del, sizeof(x->tp_del));

    // not const in AOT mode:
    // unsigned long tp_version_tag;

    MARKCONST(&x->tp_finalize, sizeof(x->tp_finalize));

    // not const in AOT mode:
    // addImmortalTuple(x->tp_mro);
}
}
