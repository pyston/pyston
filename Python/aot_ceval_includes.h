
#ifndef Py_INTERNAL_AOT_CEVAL_INCLUDES_H
#define Py_INTERNAL_AOT_CEVAL_INCLUDES_H
#ifdef __cplusplus
extern "C" {
#endif

/* enable more aggressive intra-module optimizations, where available */
#define PY_LOCAL_AGGRESSIVE
#include "Python.h"

#if defined(PYSTON_LITE) && PY_MAJOR_VERSION == 3 &&  PY_MINOR_VERSION == 7
#include "internal/pystate.h"
// make sure this points to the Pyston version of this file:
#include "../../Include/internal/pycore_code.h"
#include "../pyston/pyston_lite/compat37.h"
#else
#include "pycore_ceval.h"
#ifdef PYSTON_LITE
// make sure this points to the Pyston version of this file:
#include "../../Include/internal/pycore_code.h"
#else
#include "pycore_code.h"
#endif
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"
#endif

#include "code.h"
#include "dictobject.h"
#include "frameobject.h"
#include "opcode.h"
#ifdef PYSTON_LITE
#undef WITH_DTRACE
#endif
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"

#ifdef __cplusplus
}
#endif
#endif
