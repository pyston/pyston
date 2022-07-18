#include "Python.h"

#include "frameobject.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_tupleobject.h"

#define SET_JIT_AOT_FUNC(dst_addr) do { \
    /* retrieve address of the instruction following the call instruction */\
    unsigned char* ret_addr = (unsigned char*)__builtin_extract_return_addr(__builtin_return_address(0));\
    if (ret_addr[-2] == 0xff && ret_addr[-1] == 0xd0) { /* abs call: call rax */\
        unsigned long* call_imm = (unsigned long*)&ret_addr[-2-8];\
        *call_imm = (unsigned long)dst_addr;\
    } else { /* relative call */ \
        /* 5 byte call instruction - get address of relative immediate operand of call */\
        unsigned int* call_imm = (unsigned int*)&ret_addr[-4];\
        /* set operand to newly calculated relative offset */\
        *call_imm = (unsigned int)(unsigned long)(dst_addr) - (unsigned int)(unsigned long)ret_addr;\
    } \
} while(0)

PyObject* call_function_ceval_no_kw(PyThreadState *tstate, PyObject **stack, Py_ssize_t oparg);

PyObject *
trace_call_function(PyThreadState *tstate,
                    PyObject *func,
                    PyObject **args, Py_ssize_t nargs,
                    PyObject *kwnames);

static PyObject* _Py_HOT_FUNCTION
function_code_fastcall(PyCodeObject *co, PyObject *const *args, Py_ssize_t nargs,
                       PyObject *globals)
{
    PyFrameObject *f;
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject **fastlocals;
    Py_ssize_t i;
    PyObject *result;

    assert(globals != NULL);
    /* XXX Perhaps we should create a specialized
       _PyFrame_New_NoTrack() that doesn't take locals, but does
       take builtins without sanity checking them.
       */
    assert(tstate != NULL);
    f = _PyFrame_New_NoTrack(tstate, co, globals, NULL);
    if (f == NULL) {
        return NULL;
    }

    fastlocals = f->f_localsplus;

    for (i = 0; i < nargs; i++) {
        Py_INCREF(*args);
        fastlocals[i] = *args++;
    }
    result = PyEval_EvalFrameEx(f,0);

    if (Py_REFCNT(f) > 1) {
        Py_DECREF(f);
        _PyObject_GC_TRACK(f);
    }
    else {
#if PYSTON_SPEEDUPS
        Py_REFCNT(f) = 0;
        assert(Py_TYPE(f) == &PyFrame_Type);
        //frame_dealloc_notrashcan(f);
        PyFrame_Type.tp_dealloc(f);
#else
        ++tstate->recursion_depth;
        Py_DECREF(f);
        --tstate->recursion_depth;
#endif
    }
    return result;
}
PyObject *
_PyFunction_Vectorcall(PyObject *func, PyObject* const* stack,
                       size_t nargsf, PyObject *kwnames)
{
    PyCodeObject *co = (PyCodeObject *)PyFunction_GET_CODE(func);
    PyObject *globals = PyFunction_GET_GLOBALS(func);
    PyObject *argdefs = PyFunction_GET_DEFAULTS(func);
    PyObject *kwdefs, *closure, *name, *qualname;
    PyObject **d;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nd;

    assert(PyFunction_Check(func));
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    assert(nargs >= 0);
    assert(kwnames == NULL || PyTuple_CheckExact(kwnames));
    assert((nargs == 0 && nkwargs == 0) || stack != NULL);
    /* kwnames must only contains str strings, no subclass, and all keys must
       be unique */

    if (co->co_kwonlyargcount == 0 && nkwargs == 0 &&
#if PYSTON_SPEEDUPS
        (co->co_flags & ~(PyCF_MASK | CO_NESTED)) == (CO_OPTIMIZED | CO_NEWLOCALS | CO_NOFREE))
#else
        (co->co_flags & ~PyCF_MASK) == (CO_OPTIMIZED | CO_NEWLOCALS | CO_NOFREE))
#endif
    {
        if (
#if !PYSTON_SPEEDUPS
                argdefs == NULL &&
#endif
                co->co_argcount == nargs) {
            return function_code_fastcall(co, stack, nargs, globals);
        }
        else if (nargs == 0 && argdefs != NULL
                 && co->co_argcount == PyTuple_GET_SIZE(argdefs)) {
            /* function called with no arguments, but all parameters have
               a default value: use default values as arguments .*/
            stack = _PyTuple_ITEMS(argdefs);
            return function_code_fastcall(co, stack, PyTuple_GET_SIZE(argdefs),
                                          globals);
        }
    }

    kwdefs = PyFunction_GET_KW_DEFAULTS(func);
    closure = PyFunction_GET_CLOSURE(func);
    name = ((PyFunctionObject *)func) -> func_name;
    qualname = ((PyFunctionObject *)func) -> func_qualname;

    if (argdefs != NULL) {
        d = _PyTuple_ITEMS(argdefs);
        nd = PyTuple_GET_SIZE(argdefs);
    }
    else {
        d = NULL;
        nd = 0;
    }
    return _PyEval_EvalCodeWithName((PyObject*)co, globals, (PyObject *)NULL,
                                    stack, nargs,
                                    nkwargs ? _PyTuple_ITEMS(kwnames) : NULL,
                                    stack + nargs,
                                    nkwargs, 1,
                                    d, (int)nd, kwdefs,
                                    closure, name, qualname);
}
static inline vectorcallfunc
_PyVectorcall_FunctionFunction(PyObject *callable)
{
    return _PyFunction_Vectorcall;
}

static inline PyObject *
_PyObject_VectorcallFunction(PyObject *callable, PyObject *const *args,
                     size_t nargsf, PyObject *kwnames)
{
    PyObject *res;
    vectorcallfunc func;
    assert(kwnames == NULL || PyTuple_Check(kwnames));
    assert(args != NULL || PyVectorcall_NARGS(nargsf) == 0);
    func = _PyVectorcall_FunctionFunction(callable);
    if (func == NULL) {
        Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
        return _PyObject_MakeTpCall(callable, args, nargs, kwnames);
    }
    res = func(callable, args, nargsf, kwnames);
    return _Py_CheckFunctionResult(callable, res, NULL);
}

//Py_LOCAL_INLINE(PyObject *) _Py_HOT_FUNCTION
static PyObject *
call_functionFunction(PyThreadState *tstate, PyObject ** restrict pp_stack, Py_ssize_t oparg)
{
    PyObject *kwnames = NULL;

    PyObject **pfunc = (pp_stack) - oparg - 1;
    PyObject *func = *pfunc;
    PyObject *x, *w;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nargs = oparg - nkwargs;
    PyObject **stack = (pp_stack) - nargs - nkwargs;

    if (tstate->use_tracing) {
        x = trace_call_function(tstate, func, stack, nargs, kwnames);
    }
    else {
        x = _PyObject_VectorcallFunction(func, stack, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    }

    assert((x != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

    /* Clear the stack of the function object. */
#if !defined(LLTRACE_DEF)
    for (int i = oparg; i >= 0; i--) {
        Py_DECREF(pfunc[i]);
    }
    pp_stack = pfunc;
#else
    while ((pp_stack) > pfunc) {
        w = EXT_POP(pp_stack);
        Py_DECREF(w);
    }
#endif

    return x;
}

PyObject* call_function_ceval_no_kwProfile(PyThreadState * tstate, PyObject ** restrict stack, Py_ssize_t oparg) {
    PyObject* f = *(stack - oparg - 1);
    if (f->ob_type == &PyFunction_Type) {
        SET_JIT_AOT_FUNC(call_functionFunction);
        return call_functionFunction(tstate, stack, oparg);
    }

    SET_JIT_AOT_FUNC(call_function_ceval_no_kw);
    return call_function_ceval_no_kw(tstate, stack, oparg);
}
