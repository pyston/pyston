
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
#if PY_MAJOR_VERSION == 3 &&  PY_MINOR_VERSION <= 9
#include "pycore_tupleobject.h"
#else
#include "pycore_tuple.h"
#endif
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

// In Python <= 3.9 f_lasti and all bytecode jump offsets are always incremented by 2 for every instruction. (Because a instruction is 2 bytes long and the describe the byte offset).
// In newer versions it's instead the index of the instruction.
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION <= 9
#define INST_IDX_TO_LASTI_FACTOR 2
#else
#define INST_IDX_TO_LASTI_FACTOR 1
#endif

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
typedef struct {
    PyCodeObject *code; // The code object for the bounds. May be NULL.
    PyCodeAddressRange bounds; // Only valid if code != NULL.
    CFrame cframe;
} PyTraceInfo;

// Most opcodes support eval breaker checks but for some it's important that we don't generate them
#define OPCODE_SUPPORTS_EVAL_CHECK(opcode)  ((opcode) != SETUP_FINALLY && \
                                            (opcode) != SETUP_WITH && \
                                            (opcode) != BEFORE_ASYNC_WITH && \
                                            (opcode) != YIELD_FROM)
#endif

#ifdef __cplusplus
}
#endif
#endif
