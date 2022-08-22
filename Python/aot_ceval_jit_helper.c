// This file implements most of the more complex opcodes in the JIT
// it's mostly just copied over opcode implementation from the interpreter.
// The JIT will generate calls to this functions with a custom calling convention implemented
// using global register variables https://gcc.gnu.org/onlinedocs/gcc/Global-Register-Variables.html
// This means that we unfurtunately can only compile it with gcc and no LTO :/
// But it has the advantage that there is only a small overhead when calling into a helper
// because it has direct access to the value stack etc.
// - all functions return a PyObject*. But the yield opcodes are the only ones which return real PyObject right now.
//   But this should make it easier to replace pushes to the value stack with returns in the future.
//   Special values:
//   - 0 means jump to error
//   - 1 means continue to next opcode (only gets checked in special cases)
//   - 2 means jump to exception_unwind
// - opcodes using JUMPTO and JUMPBY need special handling in the JIT

#include "../../Python/aot_ceval_includes.h"

#include <ctype.h>

#ifdef PYSTON_LITE
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#include "aot.h"
#endif

#include "aot_ceval_jit_helper.h"

#define OPCACHE_STATS 0

#if OPCACHE_STATS
extern long loadattr_hits, loadattr_misses, loadattr_uncached, loadattr_noopcache;
extern long storeattr_hits, storeattr_misses, storeattr_uncached, storeattr_noopcache;
extern long loadmethod_hits, loadmethod_misses, loadmethod_uncached, loadmethod_noopcache;
extern long loadglobal_hits, loadglobal_misses, loadglobal_uncached, loadglobal_noopcache;
#endif

extern int _PyObject_GetMethod(PyObject *, PyObject *, PyObject **);
PyObject * do_call_core(
    PyThreadState *tstate, PyObject *func,
    PyObject *callargs, PyObject *kwdict);

void call_exc_trace(Py_tracefunc, PyObject *,
                           PyThreadState *, PyFrameObject *);

/*static*/ PyObject * cmp_outcome(PyThreadState *, int, PyObject *, PyObject *);
int import_all_from(PyThreadState *, PyObject *, PyObject *);
void format_exc_check_arg(PyThreadState *, PyObject *, const char *, PyObject *);
void format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg);
PyObject * special_lookup(PyThreadState *, PyObject *, _Py_Identifier *);
int check_args_iterable(PyThreadState *, PyObject *func, PyObject *vararg);
void format_kwargs_error(PyThreadState *, PyObject *func, PyObject *kwargs);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
void format_awaitable_error(PyThreadState *, PyTypeObject *, int);
#else
void format_awaitable_error(PyThreadState *, PyTypeObject *, int, int);
#endif

int do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);
int unpack_iterable(PyThreadState *, PyObject *, int, int, PyObject **);

#define PREDICT(x)
#define PREDICTED(x)

#define FAST_DISPATCH() return (PyObject*)1
#define DISPATCH() FAST_DISPATCH()

#define goto_error return (PyObject*)0
#define goto_exception_unwind return (PyObject*)2
#define goto_fast_block_end(retval) return (PyObject*)((unsigned long)retval | 2)

#define STACK_LEVEL()     ((int)(stack_pointer - f->f_valuestack))
#define EMPTY()           (STACK_LEVEL() == 0)
#define TOP()             (stack_pointer[-1])
#define SECOND()          (stack_pointer[-2])
#define THIRD()           (stack_pointer[-3])
#define FOURTH()          (stack_pointer[-4])
#define PEEK(n)           (stack_pointer[-(n)])
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define SET_SECOND(v)     (stack_pointer[-2] = (v))
#define SET_THIRD(v)      (stack_pointer[-3] = (v))
#define SET_FOURTH(v)     (stack_pointer[-4] = (v))
#define SET_VALUE(n, v)   (stack_pointer[-(n)] = (v))
#define BASIC_STACKADJ(n) (stack_pointer += n)
#define BASIC_PUSH(v)     (*stack_pointer++ = (v))
#define BASIC_POP()       (*--stack_pointer)

#define PUSH(v)                BASIC_PUSH(v)
#define POP()                  BASIC_POP()
#define STACK_GROW(n)          BASIC_STACKADJ(n)
#define STACK_SHRINK(n)        BASIC_STACKADJ(-n)
#define STACKADJ(n)            BASIC_STACKADJ(n)
#define EXT_POP(STACK_POINTER) (*--(STACK_POINTER))

#define UNWIND_EXCEPT_HANDLER(b) \
    do { \
        PyObject *type, *value, *traceback; \
        _PyErr_StackItem *exc_info; \
        assert(STACK_LEVEL() >= (b)->b_level + 3); \
        while (STACK_LEVEL() > (b)->b_level + 3) { \
            value = POP(); \
            Py_XDECREF(value); \
        } \
        exc_info = tstate->exc_info; \
        type = exc_info->exc_type; \
        value = exc_info->exc_value; \
        traceback = exc_info->exc_traceback; \
        exc_info->exc_type = POP(); \
        exc_info->exc_value = POP(); \
        exc_info->exc_traceback = POP(); \
        Py_XDECREF(type); \
        Py_XDECREF(value); \
        Py_XDECREF(traceback); \
    } while(0)


#define INSTR_OFFSET() (f->f_lasti + 2)

#define co f->f_code
// TODO: maybe we should cache this result to avoid looking it up all the time?!?
#define freevars (f->f_localsplus + co->co_nlocals)

// this defines our custom calling convention
// we only define the ones the helpers are currently using because else gcc will completely
// avoid using this registers even though it could safely use them
#if __aarch64__
register PyObject** stack_pointer asm("x23");
register PyFrameObject* f asm("x19");
register PyThreadState* tstate asm("x22");
#else
register PyObject** stack_pointer asm("r12");
register PyFrameObject* f asm("r13");
register PyThreadState* tstate asm("r15");
#endif

#define OPCACHE_CHECK()
#define OPCACHE_STAT_GLOBAL_HIT()
#define OPCACHE_STAT_GLOBAL_MISS()
#define OPCACHE_STAT_GLOBAL_OPT()

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG \
    "local variable '%.200s' referenced before assignment"
#define UNBOUNDFREE_ERROR_MSG \
    "free variable '%.200s' referenced before assignment" \
    " in enclosing scope"

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
// this one is special it needs to access tstate thats why it's defined here
PyObject* cmp_outcomePyCmp_EXC_MATCH(PyObject *v, PyObject *w) {
  return cmp_outcome(tstate, PyCmp_EXC_MATCH, v, w);
}
#endif

int storeAttrCache(PyObject* owner, PyObject* name, PyObject* v, _PyOpcache *co_opcache, int* err);
int setupStoreAttrCache(PyObject* owner, PyObject* name, _PyOpcache *co_opcache);
int loadAttrCache(PyObject* owner, PyObject* name, _PyOpcache *co_opcache, PyObject** res, int *meth_found);
int setupLoadAttrCache(PyObject* owner, PyObject* name, _PyOpcache *co_opcache, PyObject* res, int is_load_method, int inside_interpreter);

PyObject* _PyDict_GetItemByOffset(PyDictObject *mp, PyObject *key, Py_ssize_t dk_size, int64_t offset);

JIT_HELPER1(PRINT_EXPR, value) {
    _Py_IDENTIFIER(displayhook);
    //PyObject *value = POP();
    PyObject *hook = _PySys_GetObjectId(&PyId_displayhook);
    PyObject *res;
    if (hook == NULL) {
        _PyErr_SetString(tstate, PyExc_RuntimeError,
                            "lost sys.displayhook");
        Py_DECREF(value);
        goto_error;
    }
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
    res = PyObject_CallFunctionObjArgs(hook, value, NULL);
#else
    res = PyObject_CallOneArg(hook, value);
#endif
    Py_DECREF(value);
    if (res == NULL)
        goto_error;
    Py_DECREF(res);
    DISPATCH();
}

JIT_HELPER_WITH_OPARG(RAISE_VARARGS) {
    PyObject *cause = NULL, *exc = NULL;
    switch (oparg) {
    case 2:
        cause = POP(); /* cause */
        /* fall through */
    case 1:
        exc = POP(); /* exc */
        /* fall through */
    case 0:
        if (do_raise(tstate, exc, cause)) {
            goto_exception_unwind;
        }
        break;
    default:
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "bad RAISE_VARARGS oparg");
        break;
    }
    goto_error;
}

JIT_HELPER1(GET_AITER, obj) {
    unaryfunc getter = NULL;
    PyObject *iter = NULL;
    //PyObject *obj = POP();
    PyTypeObject *type = Py_TYPE(obj);

    if (type->tp_as_async != NULL) {
        getter = type->tp_as_async->am_aiter;
    }

    if (getter != NULL) {
        iter = (*getter)(obj);
        Py_DECREF(obj);
        if (iter == NULL) {
            goto_error;
        }
    }
    else {
        _PyErr_Format(tstate, PyExc_TypeError,
                        "'async for' requires an object with "
                        "__aiter__ method, got %.100s",
                        type->tp_name);
        Py_DECREF(obj);
        goto_error;
    }

    if (Py_TYPE(iter)->tp_as_async == NULL ||
            Py_TYPE(iter)->tp_as_async->am_anext == NULL) {

        _PyErr_Format(tstate, PyExc_TypeError,
                        "'async for' received an object from __aiter__ "
                        "that does not implement __anext__: %.100s",
                        Py_TYPE(iter)->tp_name);
        Py_DECREF(iter);
        goto_error;
    }

    return iter;
}

JIT_HELPER(GET_ANEXT) {
    unaryfunc getter = NULL;
    PyObject *next_iter = NULL;
    PyObject *awaitable = NULL;
    PyObject *aiter = TOP();
    PyTypeObject *type = Py_TYPE(aiter);

    if (PyAsyncGen_CheckExact(aiter)) {
        awaitable = type->tp_as_async->am_anext(aiter);
        if (awaitable == NULL) {
            goto_error;
        }
    } else {
        if (type->tp_as_async != NULL){
            getter = type->tp_as_async->am_anext;
        }

        if (getter != NULL) {
            next_iter = (*getter)(aiter);
            if (next_iter == NULL) {
                goto_error;
            }
        }
        else {
            _PyErr_Format(tstate, PyExc_TypeError,
                            "'async for' requires an iterator with "
                            "__anext__ method, got %.100s",
                            type->tp_name);
            goto_error;
        }

        awaitable = _PyCoro_GetAwaitableIter(next_iter);
        if (awaitable == NULL) {
            _PyErr_FormatFromCause(
                PyExc_TypeError,
                "'async for' received an invalid object "
                "from __anext__: %.100s",
                Py_TYPE(next_iter)->tp_name);

            Py_DECREF(next_iter);
            goto_error;
        } else {
            Py_DECREF(next_iter);
        }
    }

    //PUSH(awaitable);
    //PREDICT(LOAD_CONST);
    //DISPATCH();
    return awaitable;
}

JIT_HELPER1(GET_AWAITABLE, iterable) {
    PREDICTED(GET_AWAITABLE);
    //PyObject *iterable = POP();
    PyObject *iter = _PyCoro_GetAwaitableIter(iterable);

    if (iter == NULL) {
        const _Py_CODEUNIT *first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
        const _Py_CODEUNIT *next_instr = &first_instr[INSTR_OFFSET()/2];
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
        format_awaitable_error(tstate, Py_TYPE(iterable),
                                _Py_OPCODE(next_instr[-2]));
#else
        int opcode_at_minus_3 = 0;
        if ((next_instr - first_instr) > 2) {
            opcode_at_minus_3 = _Py_OPCODE(next_instr[-3]);
        }
        format_awaitable_error(tstate, Py_TYPE(iterable),
                                opcode_at_minus_3,
                                _Py_OPCODE(next_instr[-2]));
#endif
    }

    Py_DECREF(iterable);

    if (iter != NULL && PyCoro_CheckExact(iter)) {
        PyObject *yf = _PyGen_yf((PyGenObject*)iter);
        if (yf != NULL) {
            /* `iter` is a coroutine object that is being
                awaited, `yf` is a pointer to the current awaitable
                being awaited on. */
            Py_DECREF(yf);
            Py_CLEAR(iter);
            _PyErr_SetString(tstate, PyExc_RuntimeError,
                                "coroutine is being awaited already");
            /* The code below jumps to `error` if `iter` is NULL. */
        }
    }

    return iter;
}

JIT_HELPER1(YIELD_FROM, v) {
    PyObject* retval = NULL;
    //PyObject *v = POP();
    PyObject *receiver = TOP();
    int err;
    if (PyGen_CheckExact(receiver) || PyCoro_CheckExact(receiver)) {
        retval = _PyGen_Send((PyGenObject *)receiver, v);
    } else {
        _Py_IDENTIFIER(send);
        if (v == Py_None)
            retval = Py_TYPE(receiver)->tp_iternext(receiver);
        else
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
            retval = _PyObject_CallMethodIdObjArgs(receiver, &PyId_send, v, NULL);
#else
            retval = _PyObject_CallMethodIdOneArg(receiver, &PyId_send, v);
#endif
    }
    Py_DECREF(v);
    if (retval == NULL) {
        PyObject *val;
        if (tstate->c_tracefunc != NULL
                && _PyErr_ExceptionMatches(tstate, PyExc_StopIteration))
            call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
        err = _PyGen_FetchStopIterationValue(&val);
        if (err < 0)
            goto_error;
        Py_DECREF(receiver);
        SET_TOP(val);
        DISPATCH();
    }
    /* receiver remains on stack, retval is value to be yielded */
    f->f_stacktop = stack_pointer;
    /* and repeat... */
    assert(f->f_lasti >= (int)sizeof(_Py_CODEUNIT));
    f->f_lasti -= sizeof(_Py_CODEUNIT);
    // goto exit_yielding;
    return retval;
}

JIT_HELPER(POP_EXCEPT) {
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
    PyTryBlock *b = PyFrame_BlockPop(f);
    if (b->b_type != EXCEPT_HANDLER) {
        PyErr_SetString(PyExc_SystemError,
                        "popped block is not an except handler");
        goto_error;
    }
    UNWIND_EXCEPT_HANDLER(b);
    DISPATCH();
#else
    PyObject *type, *value, *traceback;
    _PyErr_StackItem *exc_info;
    PyTryBlock *b = PyFrame_BlockPop(f);
    if (b->b_type != EXCEPT_HANDLER) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "popped block is not an except handler");
        goto_error;
    }
    assert(STACK_LEVEL() >= (b)->b_level + 3 &&
            STACK_LEVEL() <= (b)->b_level + 4);
    exc_info = tstate->exc_info;
    type = exc_info->exc_type;
    value = exc_info->exc_value;
    traceback = exc_info->exc_traceback;
    exc_info->exc_type = POP();
    exc_info->exc_value = POP();
    exc_info->exc_traceback = POP();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    DISPATCH();
#endif
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
#define UNWIND_BLOCK(b) \
    while (STACK_LEVEL() > (b)->b_level) { \
        PyObject *v = POP(); \
        Py_XDECREF(v); \
    }
void JIT_HELPER_POP_BLOCK37(void) {
    PyTryBlock *b = PyFrame_BlockPop(f);
    UNWIND_BLOCK(b);
}
#endif


JIT_HELPER_WITH_OPARG(POP_FINALLY) {
    /* If oparg is 0 at the top of the stack are 1 or 6 values:
        Either:
        - TOP = NULL or an integer
        or:
        - (TOP, SECOND, THIRD) = exc_info()
        - (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER

        If oparg is 1 the value for 'return' was additionally pushed
        at the top of the stack.
    */
    PyObject *res = NULL;
    if (oparg) {
        res = POP();
    }
    PyObject *exc = POP();
    if (exc == NULL || PyLong_CheckExact(exc)) {
        Py_XDECREF(exc);
    }
    else {
        Py_DECREF(exc);
        Py_DECREF(POP());
        Py_DECREF(POP());

        PyObject *type, *value, *traceback;
        _PyErr_StackItem *exc_info;
        PyTryBlock *b = PyFrame_BlockPop(f);
        if (b->b_type != EXCEPT_HANDLER) {
            _PyErr_SetString(tstate, PyExc_SystemError,
                                "popped block is not an except handler");
            Py_XDECREF(res);
            goto_error;
        }
        assert(STACK_LEVEL() == (b)->b_level + 3);
        exc_info = tstate->exc_info;
        type = exc_info->exc_type;
        value = exc_info->exc_value;
        traceback = exc_info->exc_traceback;
        exc_info->exc_type = POP();
        exc_info->exc_value = POP();
        exc_info->exc_traceback = POP();
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }
    if (oparg) {
        PUSH(res);
    }
    DISPATCH();
}

JIT_HELPER1(END_ASYNC_FOR, exc) {
    //PyObject *exc = POP();
    assert(PyExceptionClass_Check(exc));
    if (PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration)) {
        PyTryBlock *b = PyFrame_BlockPop(f);
        assert(b->b_type == EXCEPT_HANDLER);
        Py_DECREF(exc);
        UNWIND_EXCEPT_HANDLER(b);
        Py_DECREF(POP());
        //JUMPBY(oparg);
        FAST_DISPATCH();
    }
    else {
        PyObject *val = POP();
        PyObject *tb = POP();
        _PyErr_Restore(tstate, exc, val, tb);
        goto_exception_unwind;
    }
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
PyObject* JIT_HELPER_END_FINALLY37(enum why_code* why) {
    PyObject* retval = NULL;

    PyObject *status = POP();
    if (PyLong_Check(status)) {
        *why = (enum why_code) PyLong_AS_LONG(status);
        assert(why != WHY_YIELD && why != WHY_EXCEPTION);
        if (*why == WHY_RETURN ||
            *why == WHY_CONTINUE)
            retval = POP();
        if (*why == WHY_SILENCED) {
            /* An exception was silenced by 'with', we must
            manually unwind the EXCEPT_HANDLER block which was
            created when the exception was caught, otherwise
            the stack will be in an inconsistent state. */
            PyTryBlock *b = PyFrame_BlockPop(f);
            assert(b->b_type == EXCEPT_HANDLER);
            UNWIND_EXCEPT_HANDLER(b);
            *why = WHY_NOT;
            Py_DECREF(status);
            DISPATCH();
        }
        Py_DECREF(status);
        //goto fast_block_end;
        goto_fast_block_end(retval);
    }
    else if (PyExceptionClass_Check(status)) {
        PyObject *exc = POP();
        PyObject *tb = POP();
        PyErr_Restore(status, exc, tb);
        *why = WHY_EXCEPTION;
        //goto fast_block_end;
        goto_fast_block_end(retval);
    }
    else if (status != Py_None) {
        PyErr_SetString(PyExc_SystemError,
            "'finally' pops bad exception");
        Py_DECREF(status);
        goto_error;
    }
    Py_DECREF(status);
    DISPATCH();
}
#endif

JIT_HELPER(LOAD_BUILD_CLASS) {
    _Py_IDENTIFIER(__build_class__);

    PyObject *bc;
    if (PyDict_CheckExact(f->f_builtins)) {
        bc = _PyDict_GetItemIdWithError(f->f_builtins, &PyId___build_class__);
        if (bc == NULL) {
            if (!_PyErr_Occurred(tstate)) {
                _PyErr_SetString(tstate, PyExc_NameError,
                                    "__build_class__ not found");
            }
            goto_error;
        }
        Py_INCREF(bc);
    }
    else {
        PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
        if (build_class_str == NULL)
            goto_error;
        bc = PyObject_GetItem(f->f_builtins, build_class_str);
        if (bc == NULL) {
            if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                _PyErr_SetString(tstate, PyExc_NameError,
                                    "__build_class__ not found");
            goto_error;
        }
    }
    //PUSH(bc);
    //DISPATCH();
    return bc;
}

JIT_HELPER_WITH_NAME1(STORE_NAME, v) {
    //PyObject *name = GETITEM(names, oparg);
    //PyObject *v = POP();
    PyObject *ns = f->f_locals;
    int err;
    if (ns == NULL) {
        _PyErr_Format(tstate, PyExc_SystemError,
                        "no locals found when storing %R", name);
        Py_DECREF(v);
        goto_error;
    }
    if (PyDict_CheckExact(ns))
        err = PyDict_SetItem(ns, name, v);
    else
        err = PyObject_SetItem(ns, name, v);
    Py_DECREF(v);
    if (err != 0)
        goto_error;
    DISPATCH();
}

JIT_HELPER_WITH_NAME(DELETE_NAME) {
    //PyObject *name = GETITEM(names, oparg);
    PyObject *ns = f->f_locals;
    int err;
    if (ns == NULL) {
        _PyErr_Format(tstate, PyExc_SystemError,
                        "no locals when deleting %R", name);
        goto_error;
    }
    err = PyObject_DelItem(ns, name);
    if (err != 0) {
        format_exc_check_arg(tstate, PyExc_NameError,
                                NAME_ERROR_MSG,
                                name);
        goto_error;
    }
    DISPATCH();
}

JIT_HELPER_WITH_OPARG1(UNPACK_SEQUENCE, seq) {
    PREDICTED(UNPACK_SEQUENCE);
    PyObject /**seq = POP(),*/ *item, **items;
    if (PyTuple_CheckExact(seq) &&
        PyTuple_GET_SIZE(seq) == oparg) {
        items = ((PyTupleObject *)seq)->ob_item;
        while (oparg--) {
            item = items[oparg];
            Py_INCREF(item);
            PUSH(item);
        }
    } else if (PyList_CheckExact(seq) &&
                PyList_GET_SIZE(seq) == oparg) {
        items = ((PyListObject *)seq)->ob_item;
        while (oparg--) {
            item = items[oparg];
            Py_INCREF(item);
            PUSH(item);
        }
    } else if (unpack_iterable(tstate, seq, oparg, -1,
                                stack_pointer + oparg)) {
        STACK_GROW(oparg);
    } else {
        /* unpack_iterable() raised an exception */
        Py_DECREF(seq);
        goto_error;
    }
    Py_DECREF(seq);
    DISPATCH();
}
__attribute__((flatten)) JIT_HELPER1(UNPACK_SEQUENCE2, seq) {
    return JIT_HELPER_UNPACK_SEQUENCE(2, seq);
}
__attribute__((flatten)) JIT_HELPER1(UNPACK_SEQUENCE3, seq) {
    return JIT_HELPER_UNPACK_SEQUENCE(3, seq);
}

JIT_HELPER_WITH_OPARG1(UNPACK_EX, seq) {
    int totalargs = 1 + (oparg & 0xFF) + (oparg >> 8);
    //PyObject *seq = POP();

    if (unpack_iterable(tstate, seq, oparg & 0xFF, oparg >> 8,
                        stack_pointer + totalargs)) {
        stack_pointer += totalargs;
    } else {
        Py_DECREF(seq);
        goto_error;
    }
    Py_DECREF(seq);
    DISPATCH();
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR, owner, v) {
    //PyObject *name = GETITEM(names, oparg);
    //PyObject *owner = TOP();
    //PyObject *v = SECOND();
#if OPCACHE_STATS
    storeattr_uncached++;
#endif
    int err;
    //STACK_SHRINK(2);
    err = PyObject_SetAttr(owner, name, v);
    Py_DECREF(v);
    Py_DECREF(owner);
    if (err != 0)
        goto_error;
    DISPATCH();
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR_CACHED, owner, v) {
    //PyObject *name = GETITEM(names, oparg);
    //PyObject *owner = TOP();
    //PyObject *v = SECOND();
    int err;
    //OPCACHE_FETCH();
    if (likely(v)) {
        if (likely(storeAttrCache(owner, name, v, co_opcache, &err) == 0)) {
            //STACK_SHRINK(2);
            goto sa_common;
        }

        if (++co_opcache->num_failed >= 3) {
            // don't use the cache anymore
            SET_JIT_AOT_FUNC(JIT_HELPER_STORE_ATTR);
        }
    }
    //STACK_SHRINK(2);
    err = PyObject_SetAttr(owner, name, v);

#if OPCACHE_STATS
    storeattr_misses++;
#endif

    if (err == 0) {
        if (setupStoreAttrCache(owner, name, co_opcache)) {
            // don't use the cache anymore
            SET_JIT_AOT_FUNC(JIT_HELPER_STORE_ATTR);
        }
    }

sa_common:
    Py_DECREF(v);
    Py_DECREF(owner);
    if (err != 0)
        goto_error;
    DISPATCH();
}

JIT_HELPER_WITH_NAME(DELETE_GLOBAL) {
    //PyObject *name = GETITEM(names, oparg);
    int err;
    err = PyDict_DelItem(f->f_globals, name);
    if (err != 0) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
            format_exc_check_arg(tstate, PyExc_NameError,
                                    NAME_ERROR_MSG, name);
        }
        goto_error;
    }
    DISPATCH();
}

JIT_HELPER_WITH_NAME(LOAD_NAME) {
    //PyObject *name = GETITEM(names, oparg);
    PyObject *locals = f->f_locals;
    PyObject *v;
    if (locals == NULL) {
        _PyErr_Format(tstate, PyExc_SystemError,
                        "no locals when loading %R", name);
        goto_error;
    }
    if (PyDict_CheckExact(locals)) {
        v = PyDict_GetItemWithError(locals, name);
        if (v != NULL) {
            Py_INCREF(v);
        }
        else if (_PyErr_Occurred(tstate)) {
            goto_error;
        }
    }
    else {
        v = PyObject_GetItem(locals, name);
        if (v == NULL) {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                goto_error;
            _PyErr_Clear(tstate);
        }
    }
    if (v == NULL) {
        v = PyDict_GetItemWithError(f->f_globals, name);
        if (v != NULL) {
            Py_INCREF(v);
        }
        else if (_PyErr_Occurred(tstate)) {
            goto_error;
        }
        else {
            if (PyDict_CheckExact(f->f_builtins)) {
                v = PyDict_GetItemWithError(f->f_builtins, name);
                if (v == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        format_exc_check_arg(
                                tstate, PyExc_NameError,
                                NAME_ERROR_MSG, name);
                    }
                    goto_error;
                }
                Py_INCREF(v);
            }
            else {
                v = PyObject_GetItem(f->f_builtins, name);
                if (v == NULL) {
                    if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        format_exc_check_arg(
                                    tstate, PyExc_NameError,
                                    NAME_ERROR_MSG, name);
                    }
                    goto_error;
                }
            }
        }
    }
    //PUSH(v);
    //DISPATCH();
    return v;
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_GLOBAL) {
    // assumes PyDict_CheckExact(f->f_globals) && PyDict_CheckExact(f->f_builtins))
    // otherwise we would not enter the JIT

    //PyObject *name;
    PyObject *v;
    OPCACHE_CHECK();
    if (co_opcache != NULL && co_opcache->optimized > 0) {
        _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

        PyObject *ptr = NULL;
        if (lg->cache_type == LG_GLOBAL) {
            if (lg->u.global_cache.globals_ver == ((PyDictObject *)f->f_globals)->ma_version_tag)
                ptr = lg->u.global_cache.ptr;
        } else if (lg->cache_type == LG_BUILTIN) {
            if (lg->u.builtin_cache.globals_ver == ((PyDictObject *)f->f_globals)->ma_version_tag &&
                    lg->u.builtin_cache.builtins_ver == ((PyDictObject *)f->f_builtins)->ma_version_tag)
                ptr = lg->u.builtin_cache.ptr;
        } else if (lg->cache_type == LG_GLOBAL_OFFSET) {
            ptr = _PyDict_GetItemByOffset((PyDictObject*)f->f_globals, name, lg->u.global_offset_cache.dk_size, lg->u.global_offset_cache.offset);
        } else {
            abort();
        }

        if (ptr)
        {
            OPCACHE_STAT_GLOBAL_HIT();
            assert(ptr != NULL);
            Py_INCREF(ptr);
            //PUSH(ptr);
            //DISPATCH();
            return ptr;
        }
    }

    //name = GETITEM(names, oparg);
    // Note: unlike the interpreter, we don't do the "was this from the
    // globals" optimization here.  The interpreter keeps track of this
    // so that we can jit out a better inline cache, but other than that
    // it is just slightly extra work.
    v = _PyDict_LoadGlobal((PyDictObject *)f->f_globals,
                            (PyDictObject *)f->f_builtins,
                            name);
    if (v == NULL) {
        if (!_PyErr_OCCURRED()) {
            /* _PyDict_LoadGlobal() returns NULL without raising
                * an exception if the key doesn't exist */
            format_exc_check_arg(tstate, PyExc_NameError,
                                    NAME_ERROR_MSG, name);
        }
        goto_error;
    }

    if (co_opcache != NULL) {
        _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

        if (co_opcache->optimized == 0) {
            /* Wasn't optimized before. */
            OPCACHE_STAT_GLOBAL_OPT();
        } else {
            OPCACHE_STAT_GLOBAL_MISS();
        }

        co_opcache->optimized = 1;

        lg->cache_type = LG_BUILTIN;
        lg->u.builtin_cache.globals_ver =
            ((PyDictObject *)f->f_globals)->ma_version_tag;
        lg->u.builtin_cache.builtins_ver =
            ((PyDictObject *)f->f_builtins)->ma_version_tag;
        lg->u.builtin_cache.ptr = v; /* borrowed */
    }

    Py_INCREF(v);
    //PUSH(v);
    //DISPATCH();
    return v;
}

JIT_HELPER_WITH_OPARG(UNBOUNDLOCAL_ERROR) {
    format_exc_check_arg(
        tstate, PyExc_UnboundLocalError,
        UNBOUNDLOCAL_ERROR_MSG,
        PyTuple_GetItem(co->co_varnames, oparg)
        );
    goto_error;
}

JIT_HELPER_WITH_OPARG(LOAD_CLASSDEREF) {
    PyObject *name, *value, *locals = f->f_locals;
    Py_ssize_t idx;
    assert(locals);
    assert(oparg >= PyTuple_GET_SIZE(co->co_cellvars));
    idx = oparg - PyTuple_GET_SIZE(co->co_cellvars);
    assert(idx >= 0 && idx < PyTuple_GET_SIZE(co->co_freevars));
    name = PyTuple_GET_ITEM(co->co_freevars, idx);
    if (PyDict_CheckExact(locals)) {
        value = PyDict_GetItemWithError(locals, name);
        if (value != NULL) {
            Py_INCREF(value);
        }
        else if (_PyErr_Occurred(tstate)) {
            goto_error;
        }
    }
    else {
        value = PyObject_GetItem(locals, name);
        if (value == NULL) {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                goto_error;
            }
            _PyErr_Clear(tstate);
        }
    }
    if (!value) {
        PyObject *cell = freevars[oparg];
        value = PyCell_GET(cell);
        if (value == NULL) {
            format_exc_unbound(tstate, co, oparg);
            goto_error;
        }
        Py_INCREF(value);
    }
    //PUSH(value);
    //DISPATCH();
    return value;
}

JIT_HELPER_WITH_OPARG(BUILD_STRING) {
    PyObject *str;
    PyObject *empty = PyUnicode_New(0, 0);
    if (empty == NULL) {
        goto_error;
    }
    str = _PyUnicode_JoinArray(empty, stack_pointer - oparg, oparg);
    Py_DECREF(empty);
    if (str == NULL)
        goto_error;
    while (--oparg >= 0) {
        PyObject *item = POP();
        Py_DECREF(item);
    }
    //PUSH(str);
    //DISPATCH();
    return str;
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
JIT_HELPER_WITH_OPARG(BUILD_TUPLE_UNPACK_WITH_CALL) {
    int opcode = BUILD_TUPLE_UNPACK_WITH_CALL;
    int convert_to_tuple = opcode != BUILD_LIST_UNPACK;
    Py_ssize_t i;
    PyObject *sum = PyList_New(0);
    PyObject *return_value;

    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        PyObject *none_val;

        none_val = _PyList_Extend((PyListObject *)sum, PEEK(i));
        if (none_val == NULL) {
            if (opcode == BUILD_TUPLE_UNPACK_WITH_CALL &&
                _PyErr_ExceptionMatches(tstate, PyExc_TypeError))
            {
                check_args_iterable(tstate, PEEK(1 + oparg), PEEK(i));
            }
            Py_DECREF(sum);
            goto_error;
        }
        Py_DECREF(none_val);
    }

    if (convert_to_tuple) {
        return_value = PyList_AsTuple(sum);
        Py_DECREF(sum);
        if (return_value == NULL)
            goto_error;
    }
    else {
        return_value = sum;
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(return_value);
    //DISPATCH();
    return return_value;
}

JIT_HELPER_WITH_OPARG(BUILD_TUPLE_UNPACK) {
    int opcode = BUILD_TUPLE_UNPACK;
    int convert_to_tuple = opcode != BUILD_LIST_UNPACK;
    Py_ssize_t i;
    PyObject *sum = PyList_New(0);
    PyObject *return_value;

    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        PyObject *none_val;

        none_val = _PyList_Extend((PyListObject *)sum, PEEK(i));
        if (none_val == NULL) {
            if (opcode == BUILD_TUPLE_UNPACK_WITH_CALL &&
                _PyErr_ExceptionMatches(tstate, PyExc_TypeError))
            {
                check_args_iterable(tstate, PEEK(1 + oparg), PEEK(i));
            }
            Py_DECREF(sum);
            goto_error;
        }
        Py_DECREF(none_val);
    }

    if (convert_to_tuple) {
        return_value = PyList_AsTuple(sum);
        Py_DECREF(sum);
        if (return_value == NULL)
            goto_error;
    }
    else {
        return_value = sum;
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(return_value);
    //DISPATCH();
    return return_value;
}

JIT_HELPER_WITH_OPARG(BUILD_LIST_UNPACK) {
    int opcode = BUILD_LIST_UNPACK;
    int convert_to_tuple = opcode != BUILD_LIST_UNPACK;
    Py_ssize_t i;
    PyObject *sum = PyList_New(0);
    PyObject *return_value;

    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        PyObject *none_val;

        none_val = _PyList_Extend((PyListObject *)sum, PEEK(i));
        if (none_val == NULL) {
            if (opcode == BUILD_TUPLE_UNPACK_WITH_CALL &&
                _PyErr_ExceptionMatches(tstate, PyExc_TypeError))
            {
                check_args_iterable(tstate, PEEK(1 + oparg), PEEK(i));
            }
            Py_DECREF(sum);
            goto_error;
        }
        Py_DECREF(none_val);
    }

    if (convert_to_tuple) {
        return_value = PyList_AsTuple(sum);
        Py_DECREF(sum);
        if (return_value == NULL)
            goto_error;
    }
    else {
        return_value = sum;
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(return_value);
    //DISPATCH();
    return return_value;
}
#endif

JIT_HELPER_WITH_OPARG(BUILD_SET) {
    PyObject *set = PySet_New(NULL);
    int err = 0;
    int i;
    if (set == NULL)
        goto_error;
    for (i = oparg; i > 0; i--) {
        PyObject *item = PEEK(i);
        if (err == 0)
            err = PySet_Add(set, item);
        Py_DECREF(item);
    }
    STACK_SHRINK(oparg);
    if (err != 0) {
        Py_DECREF(set);
        goto_error;
    }
    //PUSH(set);
    //DISPATCH();
    return set;
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
JIT_HELPER_WITH_OPARG(BUILD_SET_UNPACK) {
    Py_ssize_t i;
    PyObject *sum = PySet_New(NULL);
    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        if (_PySet_Update(sum, PEEK(i)) < 0) {
            Py_DECREF(sum);
            goto_error;
        }
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(sum);
    //DISPATCH();
    return sum;
}
#endif

JIT_HELPER_WITH_OPARG(BUILD_MAP) {
    Py_ssize_t i;
    PyObject *map = _PyDict_NewPresized((Py_ssize_t)oparg);
    if (map == NULL)
        goto_error;
    for (i = oparg; i > 0; i--) {
        int err;
        PyObject *key = PEEK(2*i);
        PyObject *value = PEEK(2*i - 1);
        err = PyDict_SetItem(map, key, value);
        if (err != 0) {
            Py_DECREF(map);
            goto_error;
        }
    }

    while (oparg--) {
        Py_DECREF(POP());
        Py_DECREF(POP());
    }
    //PUSH(map);
    //DISPATCH();
    return map;
}

JIT_HELPER(SETUP_ANNOTATIONS) {
    _Py_IDENTIFIER(__annotations__);
    int err;
    PyObject *ann_dict;
    if (f->f_locals == NULL) {
        _PyErr_Format(tstate, PyExc_SystemError,
                        "no locals found when setting up annotations");
        goto_error;
    }
    /* check if __annotations__ in locals()... */
    if (PyDict_CheckExact(f->f_locals)) {
        ann_dict = _PyDict_GetItemIdWithError(f->f_locals,
                                        &PyId___annotations__);
        if (ann_dict == NULL) {
            if (_PyErr_Occurred(tstate)) {
                goto_error;
            }
            /* ...if not, create a new one */
            ann_dict = PyDict_New();
            if (ann_dict == NULL) {
                goto_error;
            }
            err = _PyDict_SetItemId(f->f_locals,
                                    &PyId___annotations__, ann_dict);
            Py_DECREF(ann_dict);
            if (err != 0) {
                goto_error;
            }
        }
    }
    else {
        /* do the same if locals() is not a dict */
        PyObject *ann_str = _PyUnicode_FromId(&PyId___annotations__);
        if (ann_str == NULL) {
            goto_error;
        }
        ann_dict = PyObject_GetItem(f->f_locals, ann_str);
        if (ann_dict == NULL) {
            if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                goto_error;
            }
            _PyErr_Clear(tstate);
            ann_dict = PyDict_New();
            if (ann_dict == NULL) {
                goto_error;
            }
            err = PyObject_SetItem(f->f_locals, ann_str, ann_dict);
            Py_DECREF(ann_dict);
            if (err != 0) {
                goto_error;
            }
        }
        else {
            Py_DECREF(ann_dict);
        }
    }
    DISPATCH();
}

JIT_HELPER_WITH_OPARG(BUILD_CONST_KEY_MAP) {
    Py_ssize_t i;
    PyObject *map;
    PyObject *keys = TOP();
    if (!PyTuple_CheckExact(keys) ||
        PyTuple_GET_SIZE(keys) != (Py_ssize_t)oparg) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "bad BUILD_CONST_KEY_MAP keys argument");
        goto_error;
    }
    map = _PyDict_NewPresized((Py_ssize_t)oparg);
    if (map == NULL) {
        goto_error;
    }
    for (i = oparg; i > 0; i--) {
        int err;
        PyObject *key = PyTuple_GET_ITEM(keys, oparg - i);
        PyObject *value = PEEK(i + 1);
        err = PyDict_SetItem(map, key, value);
        if (err != 0) {
            Py_DECREF(map);
            goto_error;
        }
    }

    Py_DECREF(POP());
    while (oparg--) {
        Py_DECREF(POP());
    }
    //PUSH(map);
    //DISPATCH();
    return map;
}

JIT_HELPER_WITH_OPARG(BUILD_MAP_UNPACK) {
    Py_ssize_t i;
    PyObject *sum = PyDict_New();
    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        PyObject *arg = PEEK(i);
        if (PyDict_Update(sum, arg) < 0) {
            if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                                "'%.200s' object is not a mapping",
                                arg->ob_type->tp_name);
            }
            Py_DECREF(sum);
            goto_error;
        }
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(sum);
    //DISPATCH();
    return sum;
}

JIT_HELPER_WITH_OPARG(BUILD_MAP_UNPACK_WITH_CALL) {
    Py_ssize_t i;
    PyObject *sum = PyDict_New();
    if (sum == NULL)
        goto_error;

    for (i = oparg; i > 0; i--) {
        PyObject *arg = PEEK(i);
        if (_PyDict_MergeEx(sum, arg, 2) < 0) {
            Py_DECREF(sum);
            format_kwargs_error(tstate, PEEK(2 + oparg), arg);
            goto_error;
        }
    }

    while (oparg--)
        Py_DECREF(POP());
    //PUSH(sum);
    //DISPATCH();
    return sum;
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR, owner) {
#if OPCACHE_STATS
    loadattr_uncached++;
#endif
    //PyObject *name = GETITEM(names, oparg);
    //PyObject *owner = POP();
    PyObject *res = PyObject_GetAttr(owner, name);
    Py_DECREF(owner);
    return res;
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR_CACHED, owner) {
    //PyObject *name = GETITEM(names, oparg);
    //PyObject *owner = POP();
    PyObject *res;

    //OPCACHE_FETCH();

    if (likely(loadAttrCache(owner, name, co_opcache, &res, 0) == 0)) {
        goto la_common;
    }

    if (++co_opcache->num_failed >= 5) {
        // don't use the cache anymore
        SET_JIT_AOT_FUNC(JIT_HELPER_LOAD_ATTR);
    }

    res = PyObject_GetAttr(owner, name);

#if OPCACHE_STATS
    loadattr_misses++;
#endif

    if (res) {
        if (setupLoadAttrCache(owner, name, co_opcache, res, 0/*= not LOAD_METHOD*/, 0 /*not inside_interpreter */)) {
            // don't use the cache anymore
            SET_JIT_AOT_FUNC(JIT_HELPER_LOAD_ATTR);
        }
    }
la_common:
    Py_DECREF(owner);
    return res;
}

JIT_HELPER1(IMPORT_STAR, from) {
    PyObject /**from = POP(),*/ *locals;
    int err;
    if (PyFrame_FastToLocalsWithError(f) < 0) {
        Py_DECREF(from);
        goto_error;
    }

    locals = f->f_locals;
    if (locals == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "no locals found during 'import *'");
        Py_DECREF(from);
        goto_error;
    }
    err = import_all_from(tstate, locals, from);
    PyFrame_LocalsToFast(f, 0);
    Py_DECREF(from);
    if (err != 0)
        goto_error;
    DISPATCH();
}

JIT_HELPER1(GET_YIELD_FROM_ITER, iterable) {
    /* before: [obj]; after [getiter(obj)] */
    //PyObject *iterable = POP();
    PyObject *iter;
    if (PyCoro_CheckExact(iterable)) {
        /* `iterable` is a coroutine */
        if (!(co->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE))) {
            /* and it is used in a 'yield from' expression of a
                regular generator. */
            Py_DECREF(iterable);
            _PyErr_SetString(tstate, PyExc_TypeError,
                                "cannot 'yield from' a coroutine object "
                                "in a non-coroutine generator");
            goto_error;
        }
    }
    else if (!PyGen_CheckExact(iterable)) {
        /* `iterable` is not a generator. */
        iter = PyObject_GetIter(iterable);
        Py_DECREF(iterable);
        return iter;
    }
    return iterable;
}

JIT_HELPER(FOR_ITER_SECOND_PART) {
    PyObject *iter = TOP();
    if (_PyErr_Occurred(tstate)) {
        if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
            goto_error;
        }
        else if (tstate->c_tracefunc != NULL) {
            call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, f);
        }
        _PyErr_Clear(tstate);
    }
    /* iterator ended normally */
    STACK_SHRINK(1);
    Py_DECREF(iter);
    // handled in asm
    // JUMPBY(oparg);
    PREDICT(POP_BLOCK);
    DISPATCH();
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
JIT_HELPER1(DICT_UPDATE_ERROR, update) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Format(tstate, PyExc_TypeError,
                        "'%.200s' object is not a mapping",
                        Py_TYPE(update)->tp_name);
    }
    Py_DECREF(update);
    goto_error;
}
JIT_HELPER2(DICT_MERGE_ERROR, update, func) {
    format_kwargs_error(tstate, func, update);
    Py_DECREF(update);
    goto_error;
}
JIT_HELPER1(LIST_EXTEND_ERROR, iterable) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        (Py_TYPE(iterable)->tp_iter == NULL && !PySequence_Check(iterable)))
    {
        _PyErr_Clear(tstate);
        _PyErr_Format(tstate, PyExc_TypeError,
                "Value after * must be an iterable, not %.200s",
                Py_TYPE(iterable)->tp_name);
    }
    Py_DECREF(iterable);
    goto_error;
}
JIT_HELPER(WITH_EXCEPT_START) {
    /* At the top of the stack are 7 values:
        - (TOP, SECOND, THIRD) = exc_info()
        - (FOURTH, FIFTH, SIXTH) = previous exception for EXCEPT_HANDLER
        - SEVENTH: the context.__exit__ bound method
        We call SEVENTH(TOP, SECOND, THIRD).
        Then we push again the TOP exception and the __exit__
        return value.
    */
    PyObject *exit_func;
    PyObject *exc, *val, *tb, *res;

    exc = TOP();
    val = SECOND();
    tb = THIRD();
    assert(exc != Py_None);
    assert(!PyLong_Check(exc));
    exit_func = PEEK(7);
    PyObject *stack[4] = {NULL, exc, val, tb};
    res = PyObject_Vectorcall(exit_func, stack + 1,
            3 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    if (res == NULL)
        goto_error;
    return res;
}
#define CANNOT_CATCH_MSG "catching classes that do not inherit from "\
                         "BaseException is not allowed"
int JIT_HELPER_EXC_MATCH(PyObject *left, PyObject *right) {
    if (PyTuple_Check(right)) {
        Py_ssize_t i, length;
        length = PyTuple_GET_SIZE(right);
        for (i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            if (!PyExceptionClass_Check(exc)) {
                _PyErr_SetString(tstate, PyExc_TypeError,
                                CANNOT_CATCH_MSG);
                //Py_DECREF(left);
                //Py_DECREF(right);
                return -1;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(right)) {
            _PyErr_SetString(tstate, PyExc_TypeError,
                            CANNOT_CATCH_MSG);
            //Py_DECREF(left);
            //Py_DECREF(right);
            return -1;
        }
    }
    return PyErr_GivenExceptionMatches(left, right);
}
#endif

JIT_HELPER(BEFORE_ASYNC_WITH) {
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 8
    _Py_IDENTIFIER(__aexit__);
    _Py_IDENTIFIER(__aenter__);

    PyObject *mgr = TOP();
    PyObject *exit = special_lookup(tstate, mgr, &PyId___aexit__),
                *enter;
    PyObject *res;
    if (exit == NULL)
        goto_error;
    SET_TOP(exit);
    enter = special_lookup(tstate, mgr, &PyId___aenter__);
    Py_DECREF(mgr);
    if (enter == NULL)
        goto_error;
#else
    _Py_IDENTIFIER(__aenter__);
    _Py_IDENTIFIER(__aexit__);
    PyObject *mgr = TOP();
    PyObject *enter = special_lookup(tstate, mgr, &PyId___aenter__);
    PyObject *res;
    if (enter == NULL) {
        goto_error;
    }
    PyObject *exit = special_lookup(tstate, mgr, &PyId___aexit__);
    if (exit == NULL) {
        Py_DECREF(enter);
        goto_error;
    }
    SET_TOP(exit);
    Py_DECREF(mgr);
#endif
    res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    //if (res == NULL)
    //    goto_error;
    //PUSH(res);
    //PREDICT(GET_AWAITABLE);
    //DISPATCH();
    return res;
}

JIT_HELPER_WITH_OPARG(SETUP_WITH) {
    _Py_IDENTIFIER(__exit__);
    _Py_IDENTIFIER(__enter__);
    PyObject *mgr = TOP();
    PyObject *enter = special_lookup(tstate, mgr, &PyId___enter__);
    PyObject *res;
    if (enter == NULL) {
        goto_error;
    }
    PyObject *exit = special_lookup(tstate, mgr, &PyId___exit__);
    if (exit == NULL) {
        Py_DECREF(enter);
        goto_error;
    }
    SET_TOP(exit);
    Py_DECREF(mgr);
    res = _PyObject_CallNoArg(enter);
    Py_DECREF(enter);
    if (res == NULL)
        goto_error;
    /* Setup the finally block before pushing the result
        of __enter__ on the stack. */
    PyFrame_BlockSetup(f, SETUP_FINALLY, INSTR_OFFSET() + oparg,
                        STACK_LEVEL());

    //PUSH(res);
    //DISPATCH();
    return res;
}

JIT_HELPER(WITH_CLEANUP_START) {
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
    /* At the top of the stack are 1-6 values indicating
        how/why we entered the finally clause:
        - TOP = None
        - (TOP, SECOND) = (WHY_{RETURN,CONTINUE}), retval
        - TOP = WHY_*; no retval below it
        - (TOP, SECOND, THIRD) = exc_info()
            (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER
        Below them is EXIT, the context.__exit__ bound method.
        In the last case, we must call
            EXIT(TOP, SECOND, THIRD)
        otherwise we must call
            EXIT(None, None, None)

        In the first three cases, we remove EXIT from the
        stack, leaving the rest in the same order.  In the
        fourth case, we shift the bottom 3 values of the
        stack down, and replace the empty spot with NULL.

        In addition, if the stack represents an exception,
        *and* the function call returns a 'true' value, we
        push WHY_SILENCED onto the stack.  END_FINALLY will
        then not re-raise the exception.  (But non-local
        gotos should still be resumed.)
    */

    PyObject* stack[3];
    PyObject *exit_func;
    PyObject *exc, *val, *tb, *res;

    val = tb = Py_None;
    exc = TOP();
    if (exc == Py_None) {
        (void)POP();
        exit_func = TOP();
        SET_TOP(exc);
    }
    else if (PyLong_Check(exc)) {
        STACKADJ(-1);
        switch (PyLong_AsLong(exc)) {
        case WHY_RETURN:
        case WHY_CONTINUE:
            /* Retval in TOP. */
            exit_func = SECOND();
            SET_SECOND(TOP());
            SET_TOP(exc);
            break;
        default:
            exit_func = TOP();
            SET_TOP(exc);
            break;
        }
        exc = Py_None;
    }
    else {
        PyObject *tp2, *exc2, *tb2;
        PyTryBlock *block;
        val = SECOND();
        tb = THIRD();
        tp2 = FOURTH();
        exc2 = PEEK(5);
        tb2 = PEEK(6);
        exit_func = PEEK(7);
        SET_VALUE(7, tb2);
        SET_VALUE(6, exc2);
        SET_VALUE(5, tp2);
        /* UNWIND_EXCEPT_HANDLER will pop this off. */
        SET_FOURTH(NULL);
        /* We just shifted the stack down, so we have
            to tell the except handler block that the
            values are lower than it expects. */
        block = &f->f_blockstack[f->f_iblock - 1];
        assert(block->b_type == EXCEPT_HANDLER);
        block->b_level--;
    }

    stack[0] = exc;
    stack[1] = val;
    stack[2] = tb;
    res = _PyObject_FastCall(exit_func, stack, 3);
    Py_DECREF(exit_func);
    if (res == NULL)
        goto_error;

    Py_INCREF(exc); /* Duplicating the exception on the stack */
    PUSH(exc);
    //PUSH(res);
    //PREDICT(WITH_CLEANUP_FINISH);
    //DISPATCH();
    return res;
#else
    /* At the top of the stack are 1 or 6 values indicating
        how/why we entered the finally clause:
        - TOP = NULL
        - (TOP, SECOND, THIRD) = exc_info()
            (FOURTH, FITH, SIXTH) = previous exception for EXCEPT_HANDLER
        Below them is EXIT, the context.__exit__ or context.__aexit__
        bound method.
        In the first case, we must call
            EXIT(None, None, None)
        otherwise we must call
            EXIT(TOP, SECOND, THIRD)

        In the first case, we remove EXIT from the
        stack, leaving TOP, and push TOP on the stack.
        Otherwise we shift the bottom 3 values of the
        stack down, replace the empty spot with NULL, and push
        None on the stack.

        Finally we push the result of the call.
    */
    PyObject *stack[3];
    PyObject *exit_func;
    PyObject *exc, *val, *tb, *res;

    val = tb = Py_None;
    exc = TOP();
    if (exc == NULL) {
        STACK_SHRINK(1);
        exit_func = TOP();
        SET_TOP(exc);
        exc = Py_None;
    }
    else {
        assert(PyExceptionClass_Check(exc));
        PyObject *tp2, *exc2, *tb2;
        PyTryBlock *block;
        val = SECOND();
        tb = THIRD();
        tp2 = FOURTH();
        exc2 = PEEK(5);
        tb2 = PEEK(6);
        exit_func = PEEK(7);
        SET_VALUE(7, tb2);
        SET_VALUE(6, exc2);
        SET_VALUE(5, tp2);
        /* UNWIND_EXCEPT_HANDLER will pop this off. */
        SET_FOURTH(NULL);
        /* We just shifted the stack down, so we have
            to tell the except handler block that the
            values are lower than it expects. */
        assert(f->f_iblock > 0);
        block = &f->f_blockstack[f->f_iblock - 1];
        assert(block->b_type == EXCEPT_HANDLER);
        assert(block->b_level > 0);
        block->b_level--;
    }

    stack[0] = exc;
    stack[1] = val;
    stack[2] = tb;
    res = _PyObject_FastCall(exit_func, stack, 3);
    Py_DECREF(exit_func);
    if (res == NULL)
        goto_error;

    Py_INCREF(exc); /* Duplicating the exception on the stack */
    PUSH(exc);
    //PUSH(res);
    //PREDICT(WITH_CLEANUP_FINISH);
    //DISPATCH();
    return res;
#endif
}

JIT_HELPER2(WITH_CLEANUP_FINISH, res, exc) {
    PREDICTED(WITH_CLEANUP_FINISH);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
    //PyObject *res = POP();
    //PyObject *exc = POP();
    int err;

    if (exc != Py_None)
        err = PyObject_IsTrue(res);
    else
        err = 0;

    Py_DECREF(res);
    Py_DECREF(exc);

    if (err < 0)
        goto_error;
    else if (err > 0) {
        /* There was an exception and a True return */
        PUSH(PyLong_FromLong((long) WHY_SILENCED));
    }
    PREDICT(END_FINALLY);
    DISPATCH();
#else
    /* TOP = the result of calling the context.__exit__ bound method
        SECOND = either None or exception type

        If SECOND is None below is NULL or the return address,
        otherwise below are 7 values representing an exception.
    */
    //PyObject *res = POP();
    //PyObject *exc = POP();
    int err;

    if (exc != Py_None)
        err = PyObject_IsTrue(res);
    else
        err = 0;

    Py_DECREF(res);
    Py_DECREF(exc);

    if (err < 0)
        goto_error;
    else if (err > 0) {
        /* There was an exception and a True return.
            * We must manually unwind the EXCEPT_HANDLER block
            * which was created when the exception was caught,
            * otherwise the stack will be in an inconsistent state.
            */
        PyTryBlock *b = PyFrame_BlockPop(f);
        assert(b->b_type == EXCEPT_HANDLER);
        UNWIND_EXCEPT_HANDLER(b);
        PUSH(NULL);
    }
    PREDICT(END_FINALLY);
    DISPATCH();
#endif
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD) {
#if OPCACHE_STATS
    loadmethod_uncached++;
#endif
    /* Designed to work in tandem with CALL_METHOD. */
    //PyObject *name = GETITEM(names, oparg);
    PyObject *obj = TOP();
    PyObject *meth = NULL;

    int meth_found = _PyObject_GetMethod(obj, name, &meth);

    if (meth == NULL) {
        /* Most likely attribute wasn't found. */
        goto_error;
    }

    if (meth_found) {
        /* We can bypass temporary bound method object.
            meth is unbound method and obj is self.

            meth | self | arg1 | ... | argN
            */
        SET_TOP(meth);
        //PUSH(obj);  // self
        return obj;
    }
    else {
        /* meth is not an unbound method (but a regular attr, or
            something was returned by a descriptor protocol).  Set
            the second element of the stack to NULL, to signal
            CALL_METHOD that it's not a method call.

            NULL | meth | arg1 | ... | argN
        */
        SET_TOP(NULL);
        Py_DECREF(obj);
        //PUSH(meth);
        return meth;
    }
    DISPATCH();
}

JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD_CACHED) {
    /* Designed to work in tandem with CALL_METHOD. */
    //PyObject *name = GETITEM(names, oparg);
    PyObject *obj = TOP();
    PyObject *meth = NULL;

    //OPCACHE_FETCH();

    int is_method;
    if (likely(loadAttrCache(obj, name, co_opcache, &meth, &is_method) == 0)) {
        if (meth == NULL) {
            goto_error;
        }
        if (is_method) {
            SET_TOP(meth);
            //PUSH(obj);
            return obj;
        } else {
            SET_TOP(NULL);
            Py_DECREF(obj);
            //PUSH(meth);
            return meth;
        }
    }
    meth = NULL;


    if (++co_opcache->num_failed >= 5) {
        // don't use the cache anymore
        SET_JIT_AOT_FUNC(JIT_HELPER_LOAD_METHOD);
    }

    int meth_found = _PyObject_GetMethod(obj, name, &meth);

#if OPCACHE_STATS
    if (co_opcache)
        loadmethod_misses++;
    else
        loadmethod_noopcache++;
#endif

    if (meth == NULL) {
        /* Most likely attribute wasn't found. */
        goto_error;
    }

    if (setupLoadAttrCache(obj, name, co_opcache, meth, 1 /*= LOAD_METHOD*/, 0 /*not inside_interpreter */)) {
        // don't use the cache anymore
        SET_JIT_AOT_FUNC(JIT_HELPER_LOAD_METHOD);
    }

    if (meth_found) {
        /* We can bypass temporary bound method object.
            meth is unbound method and obj is self.

            meth | self | arg1 | ... | argN
            */
        SET_TOP(meth);
        //PUSH(obj);  // self

        return obj;
    }
    else {
        /* meth is not an unbound method (but a regular attr, or
            something was returned by a descriptor protocol).  Set
            the second element of the stack to NULL, to signal
            CALL_METHOD that it's not a method call.

            NULL | meth | arg1 | ... | argN
        */
        SET_TOP(NULL);
        Py_DECREF(obj);
        //PUSH(meth);

        return meth;
    }
    DISPATCH();
}

JIT_HELPER_WITH_OPARG3(CALL_FUNCTION_EX_internal, kwargs, callargs, func) {
    PyObject /**func, *callargs, *kwargs = NULL,*/ *result;
    if (oparg & 0x01) {
        //kwargs = POP();
        if (!PyDict_CheckExact(kwargs)) {
            PyObject *d = PyDict_New();
            if (d == NULL)
                goto_error;
            if (_PyDict_MergeEx(d, kwargs, 2) < 0) {
                Py_DECREF(d);
                format_kwargs_error(tstate, /*SECOND()*/func, kwargs);
                Py_DECREF(kwargs);
                goto_error;
            }
            Py_DECREF(kwargs);
            kwargs = d;
        }
        assert(PyDict_CheckExact(kwargs));
    }
    //callargs = POP();
    //func = POP();
    if (!PyTuple_CheckExact(callargs)) {
        if (check_args_iterable(tstate, func, callargs) < 0) {
            Py_DECREF(callargs);
            goto_error;
        }
        Py_SETREF(callargs, PySequence_Tuple(callargs));
        if (callargs == NULL) {
            goto_error;
        }
    }
    assert(PyTuple_CheckExact(callargs));

    result = do_call_core(tstate, func, callargs, kwargs);
    Py_DECREF(func);
    Py_DECREF(callargs);
    Py_XDECREF(kwargs);

    return result;
}

__attribute__((flatten)) JIT_HELPER_WITH_OPARG2(CALL_FUNCTION_EX_NOKWARGS, callargs, func) {
    return JIT_HELPER_CALL_FUNCTION_EX_internal(0 /* oparg no kwargs */, NULL, callargs, func);
}
__attribute__((flatten)) JIT_HELPER_WITH_OPARG3(CALL_FUNCTION_EX_KWARGS, kwargs, callargs, func) {
    return JIT_HELPER_CALL_FUNCTION_EX_internal(1 /* oparg with kwargs */, kwargs, callargs, func);
}

JIT_HELPER_WITH_OPARG2(MAKE_FUNCTION, qualname, codeobj) {
    //PyObject *qualname = POP();
    //PyObject *codeobj = POP();
    PyFunctionObject *func = (PyFunctionObject *)
        PyFunction_NewWithQualName(codeobj, f->f_globals, qualname);

    Py_DECREF(codeobj);
    Py_DECREF(qualname);
    if (func == NULL) {
        goto_error;
    }

    if (oparg & 0x08) {
        assert(PyTuple_CheckExact(TOP()));
        func ->func_closure = POP();
    }
    if (oparg & 0x04) {
        assert(PyDict_CheckExact(TOP()));
        func->func_annotations = POP();
    }
    if (oparg & 0x02) {
        assert(PyDict_CheckExact(TOP()));
        func->func_kwdefaults = POP();
    }
    if (oparg & 0x01) {
        assert(PyTuple_CheckExact(TOP()));
        func->func_defaults = POP();
    }

    //PUSH((PyObject *)func);
    //DISPATCH();
    return (PyObject*)func;
}

JIT_HELPER_WITH_OPARG(FORMAT_VALUE) {
    /* Handles f-string value formatting. */
    PyObject *result;
    PyObject *fmt_spec;
    PyObject *value;
    PyObject *(*conv_fn)(PyObject *);
    int which_conversion = oparg & FVC_MASK;
    int have_fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;

    fmt_spec = have_fmt_spec ? POP() : NULL;
    value = POP();

    /* See if any conversion is specified. */
    switch (which_conversion) {
    case FVC_NONE:  conv_fn = NULL;           break;
    case FVC_STR:   conv_fn = PyObject_Str;   break;
    case FVC_REPR:  conv_fn = PyObject_Repr;  break;
    case FVC_ASCII: conv_fn = PyObject_ASCII; break;
    default:
        _PyErr_Format(tstate, PyExc_SystemError,
                        "unexpected conversion flag %d",
                        which_conversion);
        goto_error;
    }

    /* If there's a conversion function, call it and replace
        value with that result. Otherwise, just use value,
        without conversion. */
    if (conv_fn != NULL) {
        result = conv_fn(value);
        Py_DECREF(value);
        if (result == NULL) {
            Py_XDECREF(fmt_spec);
            goto_error;
        }
        value = result;
    }

    /* If value is a unicode object, and there's no fmt_spec,
        then we know the result of format(value) is value
        itself. In that case, skip calling format(). I plan to
        move this optimization in to PyObject_Format()
        itself. */
    if (PyUnicode_CheckExact(value) && fmt_spec == NULL) {
        /* Do nothing, just transfer ownership to result. */
        result = value;
    } else {
        /* Actually call format(). */
        result = PyObject_Format(value, fmt_spec);
        Py_DECREF(value);
        Py_XDECREF(fmt_spec);
        if (result == NULL) {
            goto_error;
        }
    }

    //PUSH(result);
    //DISPATCH();
    return result;
}
