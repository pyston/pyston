#include "Python.h"

#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pystate.h"

// Use the cpython version of this file:
#include "dict-common.h"
#include "frameobject.h"
#include "moduleobject.h"
#include "opcode.h"

__attribute__((visibility("hidden"))) inline PyObject * call_function_ceval_fast(
    PyThreadState *tstate, PyObject ***pp_stack,
    Py_ssize_t oparg, PyObject *kwnames);
PyObject * _Py_HOT_FUNCTION
call_function_ceval_no_kw(PyThreadState *tstate, PyObject **stack, Py_ssize_t oparg) {
    return call_function_ceval_fast(tstate, &stack, oparg, NULL /*kwnames*/);
}
PyObject * _Py_HOT_FUNCTION
call_function_ceval_kw(PyThreadState *tstate, PyObject **stack, Py_ssize_t oparg, PyObject *kwnames) {
    if (kwnames == NULL)
        __builtin_unreachable();
    return call_function_ceval_fast(tstate, &stack, oparg, kwnames);
}
PyObject* PyNumber_PowerNone(PyObject *v, PyObject *w) {
  return PyNumber_Power(v, w, Py_None);
}
PyObject* PyNumber_InPlacePowerNone(PyObject *v, PyObject *w) {
  return PyNumber_InPlacePower(v, w, Py_None);
}

PyObject *
trace_call_function(PyThreadState *tstate,
                    PyObject *func,
                    PyObject **args, Py_ssize_t nargs,
                    PyObject *kwnames);

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG \
    "local variable '%.200s' referenced before assignment"
#define UNBOUNDFREE_ERROR_MSG \
    "free variable '%.200s' referenced before assignment" \
    " in enclosing scope"

PyObject *
_PyErr_Format(PyThreadState *tstate, PyObject *exception,
              const char *format, ...);

PyObject *
_PyDict_LoadGlobalEx(PyDictObject *globals, PyDictObject *builtins, PyObject *key, int *out_wasglobal)
{
    Py_ssize_t ix;
    Py_hash_t hash;
    PyObject *value;

    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *) key)->hash) == -1)
    {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }

    /* namespace 1: globals */
    ix = globals->ma_keys->dk_lookup(globals, key, hash, &value);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix != DKIX_EMPTY && value != NULL) {
        *out_wasglobal = 1;
        return value;
    }

    /* namespace 2: builtins */
    ix = builtins->ma_keys->dk_lookup(builtins, key, hash, &value);
    if (ix < 0)
        return NULL;

    *out_wasglobal = 0;

    return value;
}

static PyObject *
call_attribute(PyObject *self, PyObject *attr, PyObject *name)
{
    PyObject *res, *descr = NULL;
    descrgetfunc f = Py_TYPE(attr)->tp_descr_get;

    if (f != NULL) {
        descr = f(attr, self, (PyObject *)(Py_TYPE(self)));
        if (descr == NULL)
            return NULL;
        else
            attr = descr;
    }
    res = PyObject_CallFunctionObjArgs(attr, name, NULL);
    Py_XDECREF(descr);
    return res;
}

PyObject *slot_tp_getattr_hook_simple_not_found(PyObject *self, PyObject *name)
{
    PyObject* res = NULL;
    if ((!PyErr_Occurred() || PyErr_ExceptionMatches(PyExc_AttributeError))) {
        PyTypeObject *tp = Py_TYPE(self);
        _Py_IDENTIFIER(__getattr__);
        PyErr_Clear();
        PyObject *getattr = _PyType_LookupId(tp, &PyId___getattr__);
        Py_INCREF(getattr);
        res = call_attribute(self, getattr, name);
        Py_DECREF(getattr);
    }
    return res;
}

PyObject *slot_tp_getattr_hook_simple(PyObject *self, PyObject *name)
{
    PyObject *res = _PyObject_GenericGetAttrWithDict(self, name, NULL, 1 /* suppress */);
    if (res == NULL)
        return slot_tp_getattr_hook_simple_not_found(self, name);
    return res;
}


typedef struct {
    PyObject_HEAD
    PyObject *md_dict;
    struct PyModuleDef *md_def;
    void *md_state;
    PyObject *md_weaklist;
    PyObject *md_name;  /* for logging purposes after md_dict is cleared */
} PyModuleObject;

static PyObject*
module_getgetattr(PyModuleObject* mod) {
    _Py_IDENTIFIER(__getattr__);
    return _PyDict_GetItemId(mod->md_dict, &PyId___getattr__);
}

PyObject* module_getattro_not_found(PyObject *_m, PyObject *name)
{
    PyModuleObject *m = (PyModuleObject*)_m;
    PyObject *mod_name, *getattr;
    PyObject* err = PyErr_Occurred();
    if (err) {
        if (!PyErr_GivenExceptionMatches(err, PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
    }

    if (m->md_dict) {
        getattr = module_getgetattr(m);
        if (getattr) {
            PyObject* stack[1] = {name};
            return _PyObject_FastCall(getattr, stack, 1);
        }
        _Py_IDENTIFIER(__name__);
        mod_name = _PyDict_GetItemId(m->md_dict, &PyId___name__);
        if (mod_name && PyUnicode_Check(mod_name)) {
            _Py_IDENTIFIER(__spec__);
            Py_INCREF(mod_name);
            PyObject *spec = _PyDict_GetItemId(m->md_dict, &PyId___spec__);
            Py_XINCREF(spec);
            if (_PyModuleSpec_IsInitializing(spec)) {
                PyErr_Format(PyExc_AttributeError,
                             "partially initialized "
                             "module '%U' has no attribute '%U' "
                             "(most likely due to a circular import)",
                             mod_name, name);
            }
            else {
                PyErr_Format(PyExc_AttributeError,
                             "module '%U' has no attribute '%U'",
                             mod_name, name);
            }
            Py_XDECREF(spec);
            Py_DECREF(mod_name);
            return NULL;
        }
    }
    PyErr_Format(PyExc_AttributeError,
                "module has no attribute '%U'", name);
    return NULL;
}

#define DK_SIZE(dk) ((dk)->dk_size)
#if SIZEOF_VOID_P > 4
#define DK_IXSIZE(dk)                          \
    (DK_SIZE(dk) <= 0xff ?                     \
        1 : DK_SIZE(dk) <= 0xffff ?            \
            2 : DK_SIZE(dk) <= 0xffffffff ?    \
                4 : sizeof(int64_t))
#else
#define DK_IXSIZE(dk)                          \
    (DK_SIZE(dk) <= 0xff ?                     \
        1 : DK_SIZE(dk) <= 0xffff ?            \
            2 : sizeof(int32_t))
#endif
#define DK_ENTRIES(dk) \
    ((PyDictKeyEntry*)(&((int8_t*)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))

#define DK_MASK(dk) (((dk)->dk_size)-1)
#define IS_POWER_OF_2(x) (((x) & (x-1)) == 0)

extern void* lookdict_split_value;
PyObject* _PyDict_GetItemByOffset(PyDictObject *mp, PyObject *key, Py_ssize_t dk_size, int64_t offset) {
    assert(PyDict_CheckExact((PyObject*)mp));
    assert(PyUnicode_CheckExact(key));
    assert(offset >= 0);

    if (mp->ma_keys->dk_size != dk_size)
        return NULL;

    if (mp->ma_keys->dk_lookup == lookdict_split_value)
        return NULL;

    PyDictKeyEntry *ep = (PyDictKeyEntry*)(mp->ma_keys->dk_indices + offset);
    if (ep->me_key != key)
        return NULL;

    return ep->me_value;
}

int64_t _PyDict_GetItemOffset(PyDictObject *mp, PyObject *key, Py_ssize_t *dk_size)
{
    Py_hash_t hash;

    assert(PyDict_CheckExact((PyObject*)mp));
    assert(PyUnicode_CheckExact(key));

    if ((hash = ((PyASCIIObject *) key)->hash) == -1)
        return -1;

    if (mp->ma_keys->dk_lookup == lookdict_split_value)
        return -1;

    // don't cache if error is set because we could overwrite it
    if (PyErr_Occurred())
        return -1;

    PyObject *value = NULL;
    Py_ssize_t ix = (mp->ma_keys->dk_lookup)(mp, key, hash, &value);
    if (ix < 0) {
        PyErr_Clear();
        return -1;
    }

    *dk_size = mp->ma_keys->dk_size;
    return (char*)(&DK_ENTRIES(mp->ma_keys)[ix]) - (char*)mp->ma_keys->dk_indices;
}


PyObject * _Py_HOT_FUNCTION
call_function_ceval_fast(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg, PyObject *kwnames)
{
    PyObject** stack_top = *pp_stack;
    PyObject **pfunc = stack_top - oparg - 1;
    PyObject *func = *pfunc;
    PyObject *x, *w;
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);
    Py_ssize_t nargs = oparg - nkwargs;
    PyObject **stack = stack_top - nargs - nkwargs;

    if (__builtin_expect(tstate->use_tracing, 0)) {
        x = trace_call_function(tstate, func, stack, nargs, kwnames);
    }
    else {
        x = _PyObject_Vectorcall(func, stack, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
    }

    assert((x != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

    /* Clear the stack of the function object. */
#if !defined(LLTRACE_DEF)
    for (int i = oparg; i >= 0; i--) {
        Py_DECREF(pfunc[i]);
    }
    *pp_stack = pfunc;
#else
    while ((*pp_stack) > pfunc) {
        w = EXT_POP(*pp_stack);
        Py_DECREF(w);
    }
#endif

    return x;
}

static PySliceObject *slice_cache = NULL;
PyObject *
PySlice_NewSteal(PyObject *start, PyObject *stop, PyObject *step) {
    PySliceObject *obj;
    if (slice_cache != NULL) {
        obj = slice_cache;
        slice_cache = NULL;
        _Py_NewReference((PyObject *)obj);
    } else {
        obj = PyObject_GC_New(PySliceObject, &PySlice_Type);
        if (obj == NULL) {
            Py_DECREF(start);
            Py_DECREF(stop);
            Py_DECREF(step);
            return NULL;
        }
    }

    obj->step = step;
    obj->start = start;
    obj->stop = stop;

    _PyObject_GC_TRACK(obj);
    return (PyObject *) obj;
}
