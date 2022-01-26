#ifndef Py_INTERNAL_TUPLEOBJECT_H
#define Py_INTERNAL_TUPLEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "tupleobject.h"

#define _PyTuple_ITEMS(op) (_PyTuple_CAST(op)->ob_item)
PyAPI_FUNC(PyObject *) _PyTuple_FromArray(PyObject *const *, Py_ssize_t);
#if PYSTON_SPEEDUPS
// Similar to _PyTuple_FromArray but does not incref the tuple contents
PyAPI_FUNC(PyObject *) _PyTuple_FromArray_Borrowed(PyObject *const *, Py_ssize_t);
// These special "borrowed" tuples must be decref'd with this function:
PyAPI_FUNC(void) _PyTuple_Decref_Borrowed(PyObject *);
#else
#define _PyTuple_FromArray_Borrowed _PyTuple_FromArray
#define _PyTuple_Decref_Borrowed Py_DECREF
#endif

#ifdef __cplusplus
}
#endif
#endif   /* !Py_INTERNAL_TUPLEOBJECT_H */
