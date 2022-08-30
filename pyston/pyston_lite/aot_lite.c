#include "Python.h"

#include "../../Python/aot_ceval_includes.h"

#include "aot_ceval_jit_helper.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)


PyObject* call_function_ceval_no_kw(PyThreadState *tstate,
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
                                    PyTraceInfo* trace_info,
#endif
                                    PyObject **stack, Py_ssize_t oparg);

PyObject *
trace_call_function(PyThreadState *tstate,
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
                    PyTraceInfo* trace_info,
#endif
                    PyObject *func,
                    PyObject **args, Py_ssize_t nargs,
                    PyObject *kwnames);

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
static PyFrameObject *free_list = NULL;
static int numfree = 0;         /* number of frames currently in free_list */
/* max value for numfree */
#define PyFrame_MAXFREELIST 200

inline PyFrameObject*
_PyFrame_New_NoTrack(PyThreadState *tstate, PyCodeObject *code,
                     PyObject *globals, PyObject *locals)
{
    _Py_IDENTIFIER(__builtins__);

    PyFrameObject *back = tstate->frame;
    PyFrameObject *f;
    PyObject *builtins;
    Py_ssize_t i;

#ifdef Py_DEBUG
    if (code == NULL || globals == NULL || !PyDict_Check(globals) ||
        (locals != NULL && !PyMapping_Check(locals))) {
        PyErr_BadInternalCall();
        return NULL;
    }
#endif
    if (back == NULL || back->f_globals != globals) {
        builtins = _PyDict_GetItemIdWithError(globals, &PyId___builtins__);

        if (builtins) {
            if (
#if PYSTON_SPEEDUPS
                    Py_TYPE(builtins) != &PyDict_Type &&
#endif
                    PyModule_Check(builtins)) {
                builtins = PyModule_GetDict(builtins);
                assert(builtins != NULL);
            }
        }
        if (builtins == NULL) {
            if (PyErr_Occurred()) {
                return NULL;
            }
            /* No builtins!              Make up a minimal one
               Give them 'None', at least. */
            builtins = PyDict_New();
            if (builtins == NULL ||
                PyDict_SetItemString(
                    builtins, "None", Py_None) < 0)
                return NULL;
        }
        else
            Py_INCREF(builtins);

    }
    else {
        /* If we share the globals, we share the builtins.
           Save a lookup and a call. */
        builtins = back->f_builtins;
        assert(builtins != NULL);
        Py_INCREF(builtins);
    }
    if (code->co_zombieframe != NULL) {
        f = code->co_zombieframe;
        code->co_zombieframe = NULL;
        _Py_NewReference((PyObject *)f);
        assert(f->f_code == code);
    }
    else {
        Py_ssize_t extras, ncells, nfrees;
        ncells = PyTuple_GET_SIZE(code->co_cellvars);
        nfrees = PyTuple_GET_SIZE(code->co_freevars);
        extras = code->co_stacksize + code->co_nlocals + ncells +
            nfrees;
        if (free_list == NULL) {
            f = PyObject_GC_NewVar(PyFrameObject, &PyFrame_Type,
            extras);
            if (f == NULL) {
                Py_DECREF(builtins);
                return NULL;
            }
        }
        else {
            assert(numfree > 0);
            --numfree;
            f = free_list;
            free_list = free_list->f_back;
            if (Py_SIZE(f) < extras) {
                PyFrameObject *new_f = PyObject_GC_Resize(PyFrameObject, f, extras);
                if (new_f == NULL) {
                    PyObject_GC_Del(f);
                    Py_DECREF(builtins);
                    return NULL;
                }
                f = new_f;
            }
            _Py_NewReference((PyObject *)f);
        }

        f->f_code = code;
        extras = code->co_nlocals + ncells + nfrees;
        f->f_valuestack = f->f_localsplus + extras;
        for (i=0; i<extras; i++)
            f->f_localsplus[i] = NULL;
        f->f_locals = NULL;
        f->f_trace = NULL;
    }
    f->f_stacktop = f->f_valuestack;
    f->f_builtins = builtins;
    Py_XINCREF(back);
    f->f_back = back;
    Py_INCREF(code);
    Py_INCREF(globals);
    f->f_globals = globals;
    /* Most functions have CO_NEWLOCALS and CO_OPTIMIZED set. */
    if ((code->co_flags & (CO_NEWLOCALS | CO_OPTIMIZED)) ==
        (CO_NEWLOCALS | CO_OPTIMIZED))
        ; /* f_locals = NULL; will be set by PyFrame_FastToLocals() */
    else if (code->co_flags & CO_NEWLOCALS) {
        locals = PyDict_New();
        if (locals == NULL) {
            Py_DECREF(f);
            return NULL;
        }
        f->f_locals = locals;
    }
    else {
        if (locals == NULL)
            locals = globals;
        Py_INCREF(locals);
        f->f_locals = locals;
    }

    f->f_lasti = -1;
    f->f_lineno = code->co_firstlineno;
    f->f_iblock = 0;
    f->f_executing = 0;
    f->f_gen = NULL;
    f->f_trace_opcodes = 0;
    f->f_trace_lines = 1;

    return f;
}

static void _Py_HOT_FUNCTION
frame_dealloc_notrashcan(PyFrameObject *f)
{
    PyObject **p, **valuestack;
    PyCodeObject *co;

    if (_PyObject_GC_IS_TRACKED(f))
        _PyObject_GC_UNTRACK(f);

    //Py_TRASHCAN_SAFE_BEGIN(f)
    /* Kill all local variables */
    valuestack = f->f_valuestack;
    for (p = f->f_localsplus; p < valuestack; p++)
        Py_CLEAR(*p);

    /* Free stack */
    if (f->f_stacktop != NULL) {
        PyObject** stacktop = f->f_stacktop;
        for (p = valuestack; p < stacktop; p++)
            Py_XDECREF(*p);
    }

    Py_XDECREF(f->f_back);
    Py_DECREF(f->f_builtins);
    Py_DECREF(f->f_globals);
    Py_CLEAR(f->f_locals);
    Py_CLEAR(f->f_trace);

    co = f->f_code;
    if (co->co_zombieframe == NULL)
        co->co_zombieframe = f;
    else if (numfree < PyFrame_MAXFREELIST) {
        ++numfree;
        f->f_back = free_list;
        free_list = f;
    }
    else
        PyObject_GC_Del(f);

    Py_DECREF(co);
    //Py_TRASHCAN_SAFE_END(f)
}

PyObject* _Py_HOT_FUNCTION
_PyEval_EvalFrame_AOT
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
(PyThreadState *tstate, PyFrameObject *f, int throwflag);
#else
(PyFrameObject *f, int throwflag);
#endif

static inline PyObject* _Py_HOT_FUNCTION
function_code_fastcall(PyCodeObject *co, PyObject *const *args, Py_ssize_t nargs,
                       PyObject *globals)
{
    PyFrameObject *f;
    PyThreadState *tstate = PyThreadState_GET();
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
#if PYSTON_SPEEDUPS
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
    result = _PyEval_EvalFrame_AOT(tstate, f, 0);
#else
    result = _PyEval_EvalFrame_AOT(f, 0);
#endif
#else
    result = PyEval_EvalFrameEx(f,0);
#endif

    if (Py_REFCNT(f) > 1) {
        Py_DECREF(f);
        _PyObject_GC_TRACK(f);
    }
    else {
#if PYSTON_SPEEDUPS
        Py_REFCNT(f) = 0;
        assert(Py_TYPE(f) == &PyFrame_Type);
        frame_dealloc_notrashcan(f);
        //PyFrame_Type.tp_dealloc(f);
#else
        ++tstate->recursion_depth;
        Py_DECREF(f);
        --tstate->recursion_depth;
#endif
    }
    return result;
}
#endif

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
static PyObject *
__PyFunction_FastCallKeywords(PyObject *func, PyObject *const *stack,
                             Py_ssize_t nargs, PyObject *kwnames)
{
    PyCodeObject *co = (PyCodeObject *)PyFunction_GET_CODE(func);
    PyObject *globals = PyFunction_GET_GLOBALS(func);
    PyObject *argdefs = PyFunction_GET_DEFAULTS(func);
    PyObject *kwdefs, *closure, *name, *qualname;
    PyObject **d;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nd;

    assert(PyFunction_Check(func));
    assert(nargs >= 0);
    assert(kwnames == NULL || PyTuple_CheckExact(kwnames));
    assert((nargs == 0 && nkwargs == 0) || stack != NULL);
    /* kwnames must only contains str strings, no subclass, and all keys must
       be unique */

    if (co->co_kwonlyargcount == 0 && nkwargs == 0 &&
        (co->co_flags & ~PyCF_MASK) == (CO_OPTIMIZED | CO_NEWLOCALS | CO_NOFREE))
    {
        if (argdefs == NULL && co->co_argcount == nargs) {
            return function_code_fastcall(co, stack, nargs, globals);
        }
        else if (nargs == 0 && argdefs != NULL
                 && co->co_argcount == PyTuple_GET_SIZE(argdefs)) {
            /* function called with no arguments, but all parameters have
               a default value: use default values as arguments .*/
            stack = &PyTuple_GET_ITEM(argdefs, 0);
            return function_code_fastcall(co, stack, PyTuple_GET_SIZE(argdefs),
                                          globals);
        }
    }

    kwdefs = PyFunction_GET_KW_DEFAULTS(func);
    closure = PyFunction_GET_CLOSURE(func);
    name = ((PyFunctionObject *)func) -> func_name;
    qualname = ((PyFunctionObject *)func) -> func_qualname;

    if (argdefs != NULL) {
        d = &PyTuple_GET_ITEM(argdefs, 0);
        nd = PyTuple_GET_SIZE(argdefs);
    }
    else {
        d = NULL;
        nd = 0;
    }
    return _PyEval_EvalCodeWithName((PyObject*)co, globals, (PyObject *)NULL,
                                    stack, nargs,
                                    nkwargs ? &PyTuple_GET_ITEM(kwnames, 0) : NULL,
                                    stack + nargs,
                                    nkwargs, 1,
                                    d, (int)nd, kwdefs,
                                    closure, name, qualname);
}
#elif PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
inline PyObject *
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
        int dofast = 0;
        if (co->co_argcount == nargs)
            dofast = 1;
        else if (nargs == 0 && argdefs != NULL
                 && co->co_argcount == PyTuple_GET_SIZE(argdefs)) {
            /* function called with no arguments, but all parameters have
               a default value: use default values as arguments .*/
            stack = _PyTuple_ITEMS(argdefs);
            nargs = PyTuple_GET_SIZE(argdefs);
            dofast = 1;
        }

        if (dofast)
            return function_code_fastcall(co, stack, nargs, globals);
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
#endif

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 8
static inline vectorcallfunc
_PyVectorcall_FunctionFunction(PyObject *callable)
{
    // Pyston change:
    // We will use the inlined function definition from here for Python 3.8 to 3.9 and
    // just call the CPython original one in newer Python versions.
    return _PyFunction_Vectorcall;
}

static inline PyObject *
_PyObject_VectorcallFunction(PyThreadState *tstate, PyObject *callable, PyObject *const *args,
                     size_t nargsf, PyObject *kwnames)
{
    PyObject *res;
    vectorcallfunc func;
    assert(kwnames == NULL || PyTuple_Check(kwnames));
    assert(args != NULL || PyVectorcall_NARGS(nargsf) == 0);
    func = _PyVectorcall_FunctionFunction(callable);
    if (func == NULL) {
        Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 8
        return _PyObject_MakeTpCall(callable, args, nargs, kwnames);
#else
        return _PyObject_MakeTpCall(tstate, callable, args, nargs, kwnames);
#endif
    }
    res = func(callable, args, nargsf, kwnames);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 8
    return _Py_CheckFunctionResult(callable, res, NULL);
#else
    return _Py_CheckFunctionResult(tstate, callable, res, NULL);
#endif
}
#endif


//Py_LOCAL_INLINE(PyObject *) _Py_HOT_FUNCTION
static PyObject *
call_functionFunction(PyThreadState *tstate,
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
                      PyTraceInfo* trace_info,
#endif
                      PyObject ** restrict pp_stack, Py_ssize_t oparg)
{
    PyObject* f = pp_stack[-oparg - 1];
    if (unlikely(!(f->ob_type == &PyFunction_Type))) {
        SET_JIT_AOT_FUNC(call_function_ceval_no_kw);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
        PyObject* ret = call_function_ceval_no_kw(tstate, pp_stack, oparg);
#else
        PyObject* ret = call_function_ceval_no_kw(tstate, trace_info, pp_stack, oparg);
#endif
        return ret;
    }

    PyObject *kwnames = NULL;

    PyObject **pfunc = (pp_stack) - oparg - 1;
    PyObject *func = *pfunc;
    PyObject *x, *w;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nargs = oparg - nkwargs;
    PyObject **stack = (pp_stack) - nargs - nkwargs;

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
    x = __PyFunction_FastCallKeywords(func, stack, nargs, kwnames);
#elif PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
    if (tstate->use_tracing) {
        x = trace_call_function(tstate, func, stack, nargs, kwnames);
    }
    else {
        x = _PyObject_VectorcallFunction(tstate, func, stack, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    }
#else
    if (trace_info->cframe.use_tracing) {
        x = trace_call_function(tstate, trace_info, func, stack, nargs, kwnames);
    }
    else {
        x = _PyObject_VectorcallFunction(tstate, func, stack, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    }
#endif

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

PyObject* call_function_ceval_no_kwProfile(PyThreadState * tstate,
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
                                           PyTraceInfo* trace_info,
#endif
                                           PyObject ** restrict stack, Py_ssize_t oparg) {
    PyObject* f = *(stack - oparg - 1);
    if (f->ob_type == &PyFunction_Type) {
        SET_JIT_AOT_FUNC(call_functionFunction);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
        return call_functionFunction(tstate, stack, oparg);
#else
        return call_functionFunction(tstate, trace_info, stack, oparg);
#endif
    }

    SET_JIT_AOT_FUNC(call_function_ceval_no_kw);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
    return call_function_ceval_no_kw(tstate, stack, oparg);
#else
    return call_function_ceval_no_kw(tstate, trace_info, stack, oparg);
#endif
}
