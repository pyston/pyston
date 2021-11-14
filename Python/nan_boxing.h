
#ifndef Py_NAN_BOXING_H
#define Py_NAN_BOXING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <Python.h>

typedef void* TaggedPointer;

int tagIsNumber(TaggedPointer _tag);
int tagIsLong(TaggedPointer _tag);
int tagIsDouble(TaggedPointer tag);
int tagIsPtr(TaggedPointer tag);
int asInt32(TaggedPointer tag);
double asDouble(TaggedPointer tag);


PyObject* tagBox(TaggedPointer tag);
void tagBoxArray(PyObject** array, long len);

TaggedPointer tagUnbox(PyObject* value);
void tagUnboxArray(PyObject** array, long len);

TaggedPointer tagAddProfile(PyObject* lhs, PyObject* rhs);
TaggedPointer tagMulProfile(PyObject* lhs, PyObject* rhs);
TaggedPointer tagSubProfile(PyObject* lhs, PyObject* rhs);
TaggedPointer tagPow(PyObject* lhs, PyObject* rhs);
#ifdef __cplusplus
}
#endif
#endif /* !Py_NAN_BOXING_H */
