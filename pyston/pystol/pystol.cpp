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

bool isNamedStructPointer(Type* t, const char* name) {
    auto p = dyn_cast<PointerType>(t);
    if (!p)
        return false;
    auto st = dyn_cast<StructType>(p->getElementType());
    if (!st)
        return false;
    return st->getName() == name;
}

bool isPyObjectPtr(Type* t) {
    return isNamedStructPointer(t, "struct._object");
}

bool isPyTypeObjectPtr(Type* t) {
    return isNamedStructPointer(t, "struct._typeobject");
}

static Knowledge* getTypeFact(Value* v, FactSet& facts) {
    auto it = facts.facts.find(LOCFOR(PyObject, ob_type));
    if (it == facts.facts.end())
        return NULL;
    return &it->second;
}

extern "C" {
PyObject* _PyEval_EvalFrameDefault(PyFrameObject*, int);
PyObject *method_vectorcall(PyObject *method, PyObject *const *args, size_t nargsf, PyObject *kwnames);
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
                } else if (type == &PyMethod_Type) {
                    // methods have fixed "vectorcall" members
                    Knowledge& k = facts[LOCFOR(PyMethodObject, vectorcall)];
                    if (!k.known_value || k.known_at) {
                        // Hack, we just need a pointer type
                        Type* t = v->getType()->getPointerTo();
                        k.known_value = eval.GVToConst(GenericValue((void*)method_vectorcall), t);
                        k.known_at = NULL;
                        changed = true;
                        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                            outs() << "Know that " << *v << " is a Method object, so the vectorcall offset has value " << *k.known_value << '\n';
                    }
                }
            }
        }

        if (fact->known_value) {
            if (auto gv = dyn_cast<GlobalVariable>(fact->known_value)) {
                PyTypeObject* type = (PyTypeObject*)GVTOP(eval.evalConstant(gv));

                // None and NotImplemented are singletons, so if we know that
                // something is of the NoneType, we know it is the None object.
                if (type == &_PyNone_Type || type == &_PyNotImplemented_Type) {
                    Knowledge& k = facts[Location()];
                    if (!k.known_value || (k.known_at && !fact->known_at)) {
                        if (type == &_PyNone_Type)
                            k.known_value = eval.GVToConst(GenericValue(Py_None), v->getType());
                        else if (type == &_PyNotImplemented_Type)
                            k.known_value = eval.GVToConst(GenericValue(Py_NotImplemented), v->getType());
                        else
                            abort();

                        k.known_at = fact->known_at;
                        changed = true;
                        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                            outs() << "Know that " << *v << " is the singleton of type " << type->tp_name << "\n";
                    }
                }
            }
        }
    }

    if (isNamedStructPointer(v->getType(), "struct._is")) {
        // _is::eval_frame
        typedef struct _is _is;
        Knowledge& k = facts[LOCFOR(_is, eval_frame)];
        // We fix eval_frame to _PyEval_EvalFrameDefault since we know we are running with that to get to this trace.
        // This means that it becomes more difficult to change the frame evaluation function a second time,
        // but I think it's ok if we don't support that.
        if (!k.known_value || k.known_at) {
            // Hack, we just need a pointer type
            Type* t = v->getType()->getPointerTo();
            k.known_value = eval.GVToConst(GenericValue((void*)_PyEval_EvalFrameDefault), t);
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

                Type* t = StructType::getTypeByName(getContext(), "struct._typeobject");

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

bool BlockMatcher::canReach(const BBVector& from, const BBVector& to, const BBVector& without) {
    llvm::DenseSet<BasicBlock*> without_set;
    for (auto BB : without)
        without_set.insert(BB);

    llvm::DenseSet<BasicBlock*> to_set;
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

// `avoid` consists of blocks that are to be ignored from the CFG.
// It should only be used in specific circumstances
bool BlockMatcher::blocksAreMatched(BasicBlock* A, BasicBlock* B,
                                    BasicBlock* avoid) {
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
vector<Instruction*>
BlockMatcher::instructionsBetween(Instruction* A, Instruction* B,
                                  BasicBlock* avoid) {
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


} // namespace pystol
using namespace pystol;

extern "C" {

extern PyCFunctionObjectWithGCHeader builtin_isinstance_obj_gc, builtin_len_obj_gc, builtin_ord_obj_gc;

void pystolGlobalPythonSetup() {
    addMallocLikeFunc("PyObject_Malloc");
    addMallocLikeFunc("PyTuple_New");
    addMallocLikeFunc("PyTuple_New_Nonzeroed");

    pystolAddConstObj(Py_True);
    pystolAddConstObj(Py_False);
    pystolAddConstObj(Py_None);
    pystolAddConstObj(Py_NotImplemented);

    pystolAddConstObj((PyObject*)&builtin_isinstance_obj_gc.obj);
    pystolAddConstObj((PyObject*)&builtin_len_obj_gc.obj);
    pystolAddConstObj((PyObject*)&builtin_ord_obj_gc.obj);

    MARKCONST((char*)_Py_SwappedOp, sizeof(_Py_SwappedOp[0]) * 6);

    addConstTypeAndConstSubclasses(&PyBaseObject_Type);

    registerFactDeriver(make_unique<PystolFactDeriver>());
    registerPassFactory([](LLVMEvaluator& eval) { return new RemoveRecursionChecksPass(); });
    registerPassFactory(createMiscOptsPass);
    registerPassFactory(createExceptionTrackingPass);
}

void pystolAddConstObj(PyObject* x) {
    int offset = offsetof(PyObject, ob_type); /* skip refcount */
    MARKNOTZERO((char*)x, offset); /* refcount is never zero but not const! */
    if (PyLong_CheckExact(x) || PyBool_Check(x))
        // implementation from 'int___sizeof___impl()'
        MARKCONST(((char*)x) + offset, offsetof(PyLongObject, ob_digit)
                                           + Py_ABS(Py_SIZE(x)) * sizeof(digit)
                                           - offset);
    else if (PyFloat_CheckExact(x))
        MARKCONST(((char*)x) + offset, sizeof(PyFloatObject) - offset);
    else if (PyCFunction_Check(x)) {
        MARKCONST(((char*)x) + offset, sizeof(PyCFunctionObject) - offset);
        MARKCONST(((PyCFunctionObject*)x)->m_ml, sizeof(PyMethodDef));
    }
    else
        MARKCONST(((char*)x) + offset, sizeof(PyObject) - offset);
}

void pystolAddConstType(PyTypeObject* x) {
    // refcount is never zero but not const!
    MARKNOTZERO(&x->ob_base.ob_base.ob_refcnt, sizeof(x->ob_base.ob_base.ob_refcnt));
    MARKCONST(&x->ob_base.ob_base.ob_type, sizeof(x->ob_base.ob_base.ob_type));

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

    MARKCONST(&x->tp_vectorcall, sizeof(x->tp_vectorcall));
}
}
