#include "Python.h"

#if PY_MINOR_VERSION == 7
#include "internal/pystate.h"
#else
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pystate.h"
#endif

// Use the cpython version of this file:
#include "dict-common.h"
#include "frameobject.h"
#include "moduleobject.h"
#include "opcode.h"

#undef _Py_Dealloc
void
_Py_Dealloc(PyObject *op)
{
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
#ifdef Py_TRACE_REFS
    _Py_ForgetReference(op);
#else
    _Py_INC_TPFREES(op);
#endif
    (*dealloc)(op);
}

PyObject* _Py_CheckFunctionResult(PyObject *callable, PyObject *result, const char *where) {
    return result;
}

PyObject * _Py_HOT_FUNCTION
call_function_ceval_no_kw(PyThreadState *tstate, PyObject **stack, Py_ssize_t oparg);
PyObject * _Py_HOT_FUNCTION
call_function_ceval_kw(PyThreadState *tstate, PyObject **stack, Py_ssize_t oparg, PyObject *kwnames);
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
#if PY_MINOR_VERSION == 7
            PyErr_Format(PyExc_AttributeError,
                        "module '%U' has no attribute '%U'", mod_name, name);
#else
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
#endif
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

PyObject* _PyDict_GetItemByOffsetSplit(PyDictObject *mp, PyObject *key, Py_ssize_t dk_size, int64_t ix) {
    assert(PyDict_CheckExact((PyObject*)mp));
    assert(PyUnicode_CheckExact(key));
    assert(offset >= 0);

    if (mp->ma_keys->dk_size != dk_size)
        return NULL;

    if (mp->ma_keys->dk_lookup != lookdict_split_value)
        return NULL;

    PyDictKeyEntry *ep = DK_ENTRIES(mp->ma_keys) + ix;
    if (ep->me_key != key)
        return NULL;

    return mp->ma_values[ix];
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

int64_t _PyDict_GetItemOffsetSplit(PyDictObject *mp, PyObject *key, Py_ssize_t *dk_size)
{
    Py_hash_t hash;

    assert(PyDict_CheckExact((PyObject*)mp));
    assert(PyUnicode_CheckExact(key));

    if ((hash = ((PyASCIIObject *) key)->hash) == -1)
        return -1;

    if (mp->ma_keys->dk_lookup != lookdict_split_value)
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
    return ix;
}

PyObject *
_PyDict_GetItemFromSplitDict(PyObject *op, Py_ssize_t index) {
    PyDictObject* mp = (PyDictObject *)op;
    assert(index >= 0);
    return mp->ma_values[index];
}

Py_ssize_t
_PyDict_GetItemIndexSplitDict(PyObject *op, PyObject *key) {
    PyDictObject* mp = (PyDictObject *)op;
    Py_hash_t hash = ((PyASCIIObject *) key)->hash;
    PyObject *value;

    // this should always be set because we will get only here if a previous lookup succeeded
    // and the keys are static unicode strings
    assert(hash != -1);

    return (mp->ma_keys->dk_lookup)(mp, key, hash, &value);
}

#define MAINTAIN_TRACKING(mp, key, value) \
    do { \
        if (!_PyObject_GC_IS_TRACKED(mp)) { \
            if (_PyObject_GC_MAY_BE_TRACKED(key) || \
                _PyObject_GC_MAY_BE_TRACKED(value)) { \
                _PyObject_GC_TRACK(mp); \
            } \
        } \
    } while(0)

#define CACHED_KEYS(tp) (((PyHeapTypeObject*)tp)->ht_cached_keys)

#ifdef DEBUG_PYDICT
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 1))
#else
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 0))
#endif

#define new_values(size) PyMem_NEW(PyObject *, size)
#define free_values(values) PyMem_FREE(values)

#define USABLE_FRACTION(n) (((n) << 1)/3)

#define PyDict_MINSIZE 8

// Pyston: I can't find a way to access CPython's pydict_global_version field,
// but it should be ok if we maintain our own counter which will never overlap with theirs.
static uint64_t pydict_global_version = 1LL << 63;
#define DICT_NEXT_VERSION() (++pydict_global_version)

// Pyston: Similarly we have to maintain our own freelist:
#define PyDict_MAXFREELIST 80
static PyDictObject *free_list[PyDict_MAXFREELIST];
static int numfree = 0;
static PyDictKeysObject *keys_free_list[PyDict_MAXFREELIST];
static int numfreekeys = 0;

static PyObject *empty_values[1] = { NULL };

static void
free_keys_object(PyDictKeysObject *keys)
{
    PyDictKeyEntry *entries = DK_ENTRIES(keys);
    Py_ssize_t i, n;
    for (i = 0, n = keys->dk_nentries; i < n; i++) {
        Py_XDECREF(entries[i].me_key);
        Py_XDECREF(entries[i].me_value);
    }
    if (keys->dk_size == PyDict_MINSIZE && numfreekeys < PyDict_MAXFREELIST) {
        keys_free_list[numfreekeys++] = keys;
        return;
    }
    PyObject_FREE(keys);
}

static inline void
dictkeys_incref(PyDictKeysObject *dk)
{
    _Py_INC_REFTOTAL;
    dk->dk_refcnt++;
}

static inline void
dictkeys_decref(PyDictKeysObject *dk)
{
    assert(dk->dk_refcnt > 0);
    _Py_DEC_REFTOTAL;
    if (--dk->dk_refcnt == 0) {
        free_keys_object(dk);
    }
}

static PyObject *
new_dict(PyDictKeysObject *keys, PyObject **values)
{
    PyDictObject *mp;
    assert(keys != NULL);
    if (numfree) {
        mp = free_list[--numfree];
        assert (mp != NULL);
        assert (Py_TYPE(mp) == &PyDict_Type);
        _Py_NewReference((PyObject *)mp);
    }
    else {
        mp = PyObject_GC_New(PyDictObject, &PyDict_Type);
        if (mp == NULL) {
            dictkeys_decref(keys);
            if (values != empty_values) {
                free_values(values);
            }
            return NULL;
        }
    }
    mp->ma_keys = keys;
    mp->ma_values = values;
    mp->ma_used = 0;
    mp->ma_version_tag = DICT_NEXT_VERSION();
    ASSERT_CONSISTENT(mp);
    return (PyObject *)mp;
}

static PyObject *
new_dict_with_shared_keys(PyDictKeysObject *keys)
{
    PyObject **values;
    Py_ssize_t i, size;

    size = USABLE_FRACTION(DK_SIZE(keys));
    values = new_values(size);
    if (values == NULL) {
        dictkeys_decref(keys);
        return PyErr_NoMemory();
    }
    for (i = 0; i < size; i++) {
        values[i] = NULL;
    }
    return new_dict(keys, values);
}

int
_PyDict_SetItemFromSplitDict(PyObject *op, PyObject *key, Py_ssize_t index, PyObject* value) {
    PyDictObject* mp = (PyDictObject *)op;
    PyObject* old_val = mp->ma_values[index];

    // we have to do the slow thing will convert splitdict to regular one
    if (old_val == NULL && mp->ma_used != index)
        return PyDict_SetItem(op, key, value);

    Py_INCREF(value);
    mp->ma_values[index] = value;

    if (old_val == NULL) {
        /* pending state */
        assert(index == mp->ma_used);
        mp->ma_used++;
    } else
        Py_DECREF(old_val);

    if (old_val != value)
        mp->ma_version_tag = DICT_NEXT_VERSION();
    MAINTAIN_TRACKING(mp, key, value);
    return 0;
}

int
_PyDict_SetItemInitialFromSplitDict(PyTypeObject *tp, PyObject **dictptr, PyObject* key, Py_ssize_t index, PyObject* value) {
    PyObject *dict;
    dictkeys_incref(CACHED_KEYS(tp));
    *dictptr = dict = new_dict_with_shared_keys(CACHED_KEYS(tp));
    if (dict == NULL)
        return -1;
    return _PyDict_SetItemFromSplitDict(dict, key, index, value);
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


_Py_IDENTIFIER(__getattribute__);

PyObject * lookup_maybe_method_cached(PyObject *self, _Py_Identifier *attrid, int *unbound, PyObject** cache_slot);

static PyObject*
call_unbound(int unbound, PyObject *func, PyObject *self,
             PyObject **args, Py_ssize_t nargs)
{
    if (unbound) {
        return _PyObject_FastCall_Prepend(func, self, args, nargs);
    }
    else {
        return _PyObject_FastCall(func, args, nargs);
    }
}

static PyObject*
_process_method(PyObject* self, PyObject* res, int* unbound) {
    if (res == NULL) {
        return NULL;
    }

#if PY_MINOR_VERSION >= 8
    if (PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
        /* Avoid temporary PyMethodObject */
        *unbound = 1;
        Py_INCREF(res);
    }
    else
#endif
    {
        *unbound = 0;
        descrgetfunc f = Py_TYPE(res)->tp_descr_get;
        if (f == NULL) {
            Py_INCREF(res);
        }
        else {
            res = f(res, self, (PyObject *)(Py_TYPE(self)));
        }
    }
    return res;
}

static PyObject *
lookup_maybe_method(PyObject *self, _Py_Identifier *attrid, int *unbound)
{
    PyObject *res = _PyType_LookupId(Py_TYPE(self), attrid);
    return _process_method(self, res, unbound);
}

static PyObject *
lookup_method(PyObject *self, _Py_Identifier *attrid, int *unbound)
{
    PyObject *res = lookup_maybe_method(self, attrid, unbound);
    if (res == NULL && !PyErr_Occurred()) {
        PyErr_SetObject(PyExc_AttributeError, attrid->object);
    }
    return res;
}

static PyObject *
call_method(PyObject *obj, _Py_Identifier *name,
            PyObject **args, Py_ssize_t nargs)
{
    int unbound;
    PyObject *func, *retval;

    func = lookup_method(obj, name, &unbound);
    if (func == NULL) {
        return NULL;
    }
    retval = call_unbound(unbound, func, obj, args, nargs);
    Py_DECREF(func);
    return retval;
}

static PyObject *
slot_tp_getattro(PyObject *self, PyObject *name)
{
    PyObject *stack[1] = {name};
    return call_method(self, &PyId___getattribute__, stack, 1);
}

// This is Pyston's modified slot_tp_getattr_hook (but renamed)
PyObject *
slot_tp_getattr_hook_complex(PyObject *self, PyObject *name)
{
    PyTypeObject *tp = Py_TYPE(self);
    PyObject *getattr, *getattribute, *res;
    _Py_IDENTIFIER(__getattr__);

    /* speed hack: we could use lookup_maybe, but that would resolve the
       method fully for each attribute lookup for classes with
       __getattr__, even when the attribute is present. So we use
       _PyType_Lookup and create the method only when needed, with
       call_attribute. */
    getattr = _PyType_LookupId(tp, &PyId___getattr__);
    if (getattr == NULL) {
        /* No __getattr__ hook: use a simpler dispatcher */
        tp->tp_getattro = slot_tp_getattro;
        return slot_tp_getattro(self, name);
    }
    Py_INCREF(getattr);
    /* speed hack: we could use lookup_maybe, but that would resolve the
       method fully for each attribute lookup for classes with
       __getattr__, even when self has the default __getattribute__
       method. So we use _PyType_Lookup and create the method only when
       needed, with call_attribute. */
    getattribute = _PyType_LookupId(tp, &PyId___getattribute__);
    if (getattribute == NULL ||
        (Py_TYPE(getattribute) == &PyWrapperDescr_Type &&
         ((PyWrapperDescrObject *)getattribute)->d_wrapped ==
         (void *)PyObject_GenericGetAttr)) {
#if PYSTON_SPEEDUPS
        // switch to version which does not check for __getattribute__
        tp->tp_getattro = slot_tp_getattr_hook_simple;
        res = _PyObject_GenericGetAttrWithDict(self, name, NULL, 1 /* suppress */);
#else
        res = PyObject_GenericGetAttr(self, name);
#endif
    } else {
        Py_INCREF(getattribute);
        res = call_attribute(self, getattribute, name);
        Py_DECREF(getattribute);
    }
#if PYSTON_SPEEDUPS
    if (res == NULL && (!PyErr_Occurred() || PyErr_ExceptionMatches(PyExc_AttributeError))) {
#else
    if (res == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
#endif
        PyErr_Clear();
        res = call_attribute(self, getattr, name);
    }
    Py_DECREF(getattr);
    return res;
}
