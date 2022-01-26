/* Tuple object interface */

#ifndef Py_TUPLEOBJECT_H
#define Py_TUPLEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/*
Another generally useful object type is a tuple of object pointers.
For Python, this is an immutable type.  C code can change the tuple items
(but not their number), and even use tuples as general-purpose arrays of
object references, but in general only brand new tuples should be mutated,
not ones that might already have been exposed to Python code.

*** WARNING *** PyTuple_SetItem does not increment the new item's reference
count, but does decrement the reference count of the item it replaces,
if not nil.  It does *decrement* the reference count if it is *not*
inserted in the tuple.  Similarly, PyTuple_GetItem does not increment the
returned item's reference count.
*/

PyAPI_DATA(PyTypeObject) PyTuple_Type;
PyAPI_DATA(PyTypeObject) PyTupleIter_Type;

#define PyTuple_Check(op) \
                 PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_TUPLE_SUBCLASS)
#define PyTuple_CheckExact(op) (Py_TYPE(op) == &PyTuple_Type)

PyAPI_FUNC(PyObject *) PyTuple_New(Py_ssize_t size);
#if PYSTON_SPEEDUPS
PyAPI_FUNC(PyObject *) PyTuple_New_Nonzeroed(Py_ssize_t size);
#else
#define PyTuple_New_Nonzeroed PyTuple_New
#endif
PyAPI_FUNC(Py_ssize_t) PyTuple_Size(PyObject *);
PyAPI_FUNC(PyObject *) PyTuple_GetItem(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyTuple_SetItem(PyObject *, Py_ssize_t, PyObject *);
PyAPI_FUNC(PyObject *) PyTuple_GetSlice(PyObject *, Py_ssize_t, Py_ssize_t);
PyAPI_FUNC(PyObject *) PyTuple_Pack(Py_ssize_t, ...);
#if PYSTON_SPEEDUPS
// Because PyTuple_Pack does no C-type-checking of its arguments,
// wrap the new Pack functions in macros that do auto-casting:
#define PyTuple_Pack1(o1) _PyTuple_Pack1((PyObject*)(o1))
#define PyTuple_Pack2(o1, o2) _PyTuple_Pack2((PyObject*)(o1), (PyObject*)(o2))
#define PyTuple_Pack3(o1, o2, o3) _PyTuple_Pack3((PyObject*)(o1), (PyObject*)(o2), (PyObject*)(o3))
PyAPI_FUNC(PyObject *) _PyTuple_Pack1(PyObject *);
PyAPI_FUNC(PyObject *) _PyTuple_Pack2(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyTuple_Pack3(PyObject *, PyObject *, PyObject *);
#else
#define PyTuple_Pack1(el0) PyTuple_Pack(1, (el0))
#define PyTuple_Pack2(el0, el1) PyTuple_Pack(2, (el0), (el1))
#define PyTuple_Pack3(el0, el1, el2) PyTuple_Pack(3, (el0), (el1), (el2))
#endif

PyAPI_FUNC(int) PyTuple_ClearFreeList(void);

#ifndef Py_LIMITED_API
#  define Py_CPYTHON_TUPLEOBJECT_H
#  include  "cpython/tupleobject.h"
#  undef Py_CPYTHON_TUPLEOBJECT_H
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_TUPLEOBJECT_H */
