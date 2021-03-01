#define PY_LOCAL_AGGRESSIVE

#include "Python.h"
#include "pycore_ceval.h"
#include "pycore_code.h"
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"
#include "pycore_tupleobject.h"

#include "code.h"
#include "../Objects/dict-common.h"
#include "dictobject.h"
#include "frameobject.h"
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"

#define DEFERRED_VS_MAX 16 /* used by STORE_SUBSCR */
#define NUM_MANUAL_STACK_SLOTS 2 /* used by STORE_SUBSCR */

static int is_immortal(PyObject* obj) {
    return obj->ob_refcnt > (1L<<59);
}

typedef enum ValueStackLoc {
    CONST,
    FAST,
    REGISTER,
    STACK,
} ValueStackLoc;

// Enum values for ref_status:
typedef enum RefStatus {
    BORROWED,
    OWNED,
    IMMORTAL,
} RefStatus;

typedef enum Section {
    SECTION_CODE,
    SECTION_COLD,
    SECTION_ENTRY,
    SECTION_OPCODE_ADDR,
} Section;

typedef struct DeferredValueStackEntry {
    ValueStackLoc loc;
    int val;
} DeferredValueStackEntry;

typedef struct Jit {
    struct dasm_State* d;
    PyCodeObject* co;
    PyObject* co_consts;
    PyObject* co_names;

    int deferred_vs_next;

    DeferredValueStackEntry deferred_vs[DEFERRED_VS_MAX];

    int num_deferred_stack_slots;
    int deferred_stack_slot_next;

    Section current_section;

    // =1 if an entry in deferred_vs is using the preserved_reg2
    int deferred_vs_reg2_used;
} Jit;

#define Dst_DECL Jit* Dst
#define Dst_REF Dst->d

#include <dynasm/dasm_proto.h>
#include <dynasm/dasm_x86.h>

#include <sys/mman.h>
#include <ctype.h>

#include "aot.h"

// used if JIT_PERF_MAP is enabled
static FILE *perf_map_file = NULL, *perf_map_opcode_map = NULL;
static long perf_map_num_funcs = 0;
struct PerfMapEntry {
    char* func_name; // must call free() on it
    void* func_addr;
    long func_size;
} *perf_map_funcs;

static int jit_use_aot = 1;

static PyObject* cmp_outcomePyCmp_IS(PyObject *v, PyObject *w) {
  return cmp_outcome(NULL, PyCmp_IS, v, w);
}
static PyObject* cmp_outcomePyCmp_IS_NOT(PyObject *v, PyObject *w) {
  return cmp_outcome(NULL, PyCmp_IS_NOT, v, w);
}
static PyObject* cmp_outcomePyCmp_BAD(PyObject *v, PyObject *w) {
  return cmp_outcome(NULL, PyCmp_BAD, v, w);
}
PyObject* cmp_outcomePyCmp_EXC_MATCH(PyObject *v, PyObject *w);

int eval_breaker_jit_helper();
PyObject* loadAttrCacheAttrNotFound(PyObject *owner, PyObject *name);

PyObject * import_name(PyThreadState *, PyFrameObject *,
                              PyObject *, PyObject *, PyObject *);
PyObject * import_from(PyThreadState *, PyObject *, PyObject *);
void format_exc_unbound(PyThreadState *tstate, PyCodeObject *co, int oparg);

Py_ssize_t lookdict_split(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr);

// DECLARATION OF THE JIT HELPER FUNCTIONS
#define JIT_HELPER(name) PyObject* JIT_HELPER_##name(int oparg)
#define JIT_HELPER1(name, py1) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1)
#define JIT_HELPER2(name, py1, py2) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1, PyObject* py2)
#define JIT_HELPER3(name, py1, py2, py3) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1, PyObject* py2, PyObject* py3)
#define JIT_HELPER_WITH_NAME(name_) PyObject* JIT_HELPER_##name_(PyObject* name)
#define JIT_HELPER_WITH_NAME1(name_, py1) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT(name_) PyObject* JIT_HELPER_##name_(int oparg, PyObject* name, _PyOpcache *co_opcache)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT1(name_, py1) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1, _PyOpcache *co_opcache)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT2(name_, py1, py2) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1, PyObject* py2, _PyOpcache *co_opcache)

JIT_HELPER1(UNARY_NOT, value);
JIT_HELPER1(PRINT_EXPR, value);
JIT_HELPER(RAISE_VARARGS);
JIT_HELPER1(GET_AITER, obj);
JIT_HELPER(GET_ANEXT);
JIT_HELPER1(GET_AWAITABLE, iterable);
JIT_HELPER1(YIELD_FROM, v);
JIT_HELPER1(YIELD_VALUE, retval);
JIT_HELPER(POP_EXCEPT);
JIT_HELPER(POP_BLOCK);
JIT_HELPER(POP_FINALLY);
JIT_HELPER(BEGIN_FINALLY);
JIT_HELPER1(END_ASYNC_FOR, exc);
JIT_HELPER(LOAD_BUILD_CLASS);
JIT_HELPER_WITH_NAME1(STORE_NAME, v);
JIT_HELPER_WITH_NAME(DELETE_NAME);
JIT_HELPER1(UNPACK_SEQUENCE, seq);
JIT_HELPER1(UNPACK_SEQUENCE2, seq);
JIT_HELPER1(UNPACK_SEQUENCE3, seq);
JIT_HELPER1(UNPACK_EX, seq);
JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR, owner, v);
JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR_CACHED, owner, v);
JIT_HELPER_WITH_NAME(DELETE_GLOBAL);
JIT_HELPER_WITH_NAME(LOAD_NAME);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_GLOBAL);
JIT_HELPER(LOAD_CLASSDEREF);
JIT_HELPER(BUILD_STRING);
JIT_HELPER(BUILD_TUPLE_UNPACK_WITH_CALL);
JIT_HELPER(BUILD_TUPLE_UNPACK);
JIT_HELPER(BUILD_LIST_UNPACK);
JIT_HELPER(BUILD_SET);
JIT_HELPER(BUILD_SET_UNPACK);
JIT_HELPER(BUILD_MAP);
JIT_HELPER(SETUP_ANNOTATIONS);
JIT_HELPER(BUILD_CONST_KEY_MAP);
JIT_HELPER(BUILD_MAP_UNPACK);
JIT_HELPER(BUILD_MAP_UNPACK_WITH_CALL);
JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR, owner);
JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR_CACHED, owner);
JIT_HELPER1(IMPORT_STAR, from);
JIT_HELPER1(GET_YIELD_FROM_ITER, iterable);
JIT_HELPER(SETUP_FINALLY);
JIT_HELPER(BEFORE_ASYNC_WITH);
JIT_HELPER1(SETUP_ASYNC_WITH, res);
JIT_HELPER(SETUP_WITH);
JIT_HELPER(WITH_CLEANUP_START);
JIT_HELPER2(WITH_CLEANUP_FINISH, res, exc);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD_CACHED);
JIT_HELPER1(CALL_FUNCTION_KW, names_);
JIT_HELPER2(CALL_FUNCTION_EX_NOKWARGS, callargs, func);
JIT_HELPER3(CALL_FUNCTION_EX_KWARGS, kwargs, callargs, func);
JIT_HELPER2(MAKE_FUNCTION, qualname, codeobj);
JIT_HELPER(FORMAT_VALUE);

// special ones created by us
JIT_HELPER(FOR_ITER_SECOND_PART);
JIT_HELPER(UNBOUNDLOCAL_ERROR);

#undef JIT_HELPER
#undef JIT_HELPER_WITH_NAME
#undef JIT_HELPER_WITH_NAME_OPCACHE_AOT

static void* __attribute__ ((const)) get_addr_of_helper_func(int opcode, int oparg) {
    switch (opcode) {
#define JIT_HELPER(name)   case name: return JIT_HELPER_##name
#define JIT_HELPER_WITH_NAME(name) case name: return JIT_HELPER_##name
        JIT_HELPER(UNARY_NOT);
        JIT_HELPER(PRINT_EXPR);
        JIT_HELPER(RAISE_VARARGS);
        JIT_HELPER(GET_AITER);
        JIT_HELPER(GET_ANEXT);
        JIT_HELPER(GET_AWAITABLE);
        JIT_HELPER(YIELD_FROM);
        JIT_HELPER(YIELD_VALUE);
        JIT_HELPER(POP_EXCEPT);
        JIT_HELPER(POP_BLOCK);
        JIT_HELPER(POP_FINALLY);
        JIT_HELPER(BEGIN_FINALLY);
        JIT_HELPER(END_ASYNC_FOR);
        JIT_HELPER(LOAD_BUILD_CLASS);
        JIT_HELPER_WITH_NAME(STORE_NAME);
        JIT_HELPER_WITH_NAME(DELETE_NAME);
        JIT_HELPER(UNPACK_EX);
        JIT_HELPER_WITH_NAME(DELETE_GLOBAL);
        JIT_HELPER_WITH_NAME(LOAD_NAME);
        JIT_HELPER(LOAD_CLASSDEREF);
        JIT_HELPER(BUILD_STRING);
        JIT_HELPER(BUILD_TUPLE_UNPACK_WITH_CALL);
        JIT_HELPER(BUILD_TUPLE_UNPACK);
        JIT_HELPER(BUILD_LIST_UNPACK);
        JIT_HELPER(BUILD_SET);
        JIT_HELPER(BUILD_SET_UNPACK);
        JIT_HELPER(BUILD_MAP);
        JIT_HELPER(SETUP_ANNOTATIONS);
        JIT_HELPER(BUILD_CONST_KEY_MAP);
        JIT_HELPER(BUILD_MAP_UNPACK);
        JIT_HELPER(BUILD_MAP_UNPACK_WITH_CALL);
        JIT_HELPER(IMPORT_STAR);
        JIT_HELPER(GET_YIELD_FROM_ITER);
        JIT_HELPER(SETUP_FINALLY);
        JIT_HELPER(BEFORE_ASYNC_WITH);
        JIT_HELPER(SETUP_ASYNC_WITH);
        JIT_HELPER(SETUP_WITH);
        JIT_HELPER(WITH_CLEANUP_START);
        JIT_HELPER(WITH_CLEANUP_FINISH);
        JIT_HELPER(CALL_FUNCTION_KW);
        JIT_HELPER(MAKE_FUNCTION);
        JIT_HELPER(FORMAT_VALUE);

        case UNPACK_SEQUENCE:
            if (oparg == 2) return JIT_HELPER_UNPACK_SEQUENCE2;
            if (oparg == 3) return JIT_HELPER_UNPACK_SEQUENCE3;
            return JIT_HELPER_UNPACK_SEQUENCE;

        case CALL_FUNCTION_EX:
            if (oparg == 0) return JIT_HELPER_CALL_FUNCTION_EX_NOKWARGS;
            if (oparg == 1) return JIT_HELPER_CALL_FUNCTION_EX_KWARGS;
            printf("could not find helper for opcode: %d oparg: %d\n", opcode, oparg);
            abort();

        default:
            printf("could not find helper for opcode: %d oparg: %d\n", opcode, oparg);
            abort();
    }
}

static void* __attribute__ ((const)) get_addr_of_aot_func(int opcode, int oparg, int opcache_available) {
    #define OPCODE_STATIC(x, func) if (opcode == x) return (func)
    #define OPCODE_PROFILE(x, func) OPCODE_STATIC(x, jit_use_aot ? func##Profile : func)

    OPCODE_PROFILE(UNARY_POSITIVE, PyNumber_Positive);
    OPCODE_PROFILE(UNARY_NEGATIVE, PyNumber_Negative);
    OPCODE_PROFILE(UNARY_INVERT, PyNumber_Invert);

    OPCODE_PROFILE(GET_ITER, PyObject_GetIter);

    OPCODE_PROFILE(BINARY_MULTIPLY, PyNumber_Multiply);
    OPCODE_PROFILE(BINARY_MATRIX_MULTIPLY, PyNumber_MatrixMultiply);
    OPCODE_PROFILE(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide);
    OPCODE_PROFILE(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide);
    OPCODE_PROFILE(BINARY_MODULO, PyNumber_Remainder);
    OPCODE_PROFILE(BINARY_ADD, PyNumber_Add);
    OPCODE_PROFILE(BINARY_SUBTRACT, PyNumber_Subtract);
    OPCODE_PROFILE(BINARY_LSHIFT, PyNumber_Lshift);
    OPCODE_PROFILE(BINARY_RSHIFT, PyNumber_Rshift);
    OPCODE_PROFILE(BINARY_AND, PyNumber_And);
    OPCODE_PROFILE(BINARY_XOR, PyNumber_Xor);
    OPCODE_PROFILE(BINARY_OR, PyNumber_Or);

    OPCODE_PROFILE(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply);
    OPCODE_PROFILE(INPLACE_MATRIX_MULTIPLY, PyNumber_InPlaceMatrixMultiply);
    OPCODE_PROFILE(INPLACE_TRUE_DIVIDE, PyNumber_InPlaceTrueDivide);
    OPCODE_PROFILE(INPLACE_FLOOR_DIVIDE, PyNumber_InPlaceFloorDivide);
    OPCODE_PROFILE(INPLACE_MODULO, PyNumber_InPlaceRemainder);
    OPCODE_PROFILE(INPLACE_ADD, PyNumber_InPlaceAdd);
    OPCODE_PROFILE(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract);
    OPCODE_PROFILE(INPLACE_LSHIFT, PyNumber_InPlaceLshift);
    OPCODE_PROFILE(INPLACE_RSHIFT, PyNumber_InPlaceRshift);
    OPCODE_PROFILE(INPLACE_AND, PyNumber_InPlaceAnd);
    OPCODE_PROFILE(INPLACE_XOR, PyNumber_InPlaceXor);
    OPCODE_PROFILE(INPLACE_OR, PyNumber_InPlaceOr);

    // the normaly have 3 args but we created special versions because last is always None
    OPCODE_PROFILE(BINARY_POWER, PyNumber_PowerNone);
    OPCODE_PROFILE(INPLACE_POWER, PyNumber_InPlacePowerNone);

    OPCODE_PROFILE(CALL_FUNCTION, call_function_ceval_no_kw);
    OPCODE_PROFILE(CALL_METHOD, call_method_ceval_no_kw);

    OPCODE_PROFILE(STORE_SUBSCR, PyObject_SetItem);
    OPCODE_PROFILE(BINARY_SUBSCR, PyObject_GetItem);
    OPCODE_PROFILE(DELETE_SUBSCR, PyObject_DelItem);

    OPCODE_STATIC(LOAD_GLOBAL, JIT_HELPER_LOAD_GLOBAL);
    if (opcache_available) {
        OPCODE_STATIC(LOAD_ATTR, JIT_HELPER_LOAD_ATTR_CACHED);
        OPCODE_STATIC(STORE_ATTR, JIT_HELPER_STORE_ATTR_CACHED);
        OPCODE_STATIC(LOAD_METHOD, JIT_HELPER_LOAD_METHOD_CACHED);
    } else {
        OPCODE_STATIC(LOAD_ATTR, JIT_HELPER_LOAD_ATTR);
        OPCODE_STATIC(STORE_ATTR, JIT_HELPER_STORE_ATTR);
        OPCODE_STATIC(LOAD_METHOD, JIT_HELPER_LOAD_METHOD);
    }

    if (opcode == COMPARE_OP) {
        switch (oparg) {
        case PyCmp_LT: return jit_use_aot ? cmp_outcomePyCmp_LTProfile : cmp_outcomePyCmp_LT;
        case PyCmp_LE: return jit_use_aot ? cmp_outcomePyCmp_LEProfile : cmp_outcomePyCmp_LE;
        case PyCmp_EQ: return jit_use_aot ? cmp_outcomePyCmp_EQProfile : cmp_outcomePyCmp_EQ;
        case PyCmp_NE: return jit_use_aot ? cmp_outcomePyCmp_NEProfile : cmp_outcomePyCmp_NE;
        case PyCmp_GT: return jit_use_aot ? cmp_outcomePyCmp_GTProfile : cmp_outcomePyCmp_GT;
        case PyCmp_GE: return jit_use_aot ? cmp_outcomePyCmp_GEProfile : cmp_outcomePyCmp_GE;
        case PyCmp_IN: return jit_use_aot ? cmp_outcomePyCmp_INProfile : cmp_outcomePyCmp_IN;
        case PyCmp_NOT_IN: return jit_use_aot ? cmp_outcomePyCmp_NOT_INProfile : cmp_outcomePyCmp_NOT_IN;

        // we don't create type specific version for those so use non Profile final versions
        case PyCmp_IS: return cmp_outcomePyCmp_IS;
        case PyCmp_IS_NOT: return cmp_outcomePyCmp_IS_NOT;
        case PyCmp_BAD: return cmp_outcomePyCmp_BAD;
        case PyCmp_EXC_MATCH: return cmp_outcomePyCmp_EXC_MATCH;
        }
    }
#undef OPCODE_STATIC
#undef OPCODE_PROFILE
    printf("could not find aot func for opcode: %d oparg: %d\n", opcode, oparg);
    abort();
    return 0;
}

static const char* get_opcode_name(int opcode) {
#define OPCODE_NAME(op) case op: return #op
    switch (opcode) {
        OPCODE_NAME(POP_TOP);
        OPCODE_NAME(ROT_TWO);
        OPCODE_NAME(ROT_THREE);
        OPCODE_NAME(DUP_TOP);
        OPCODE_NAME(DUP_TOP_TWO);
        OPCODE_NAME(ROT_FOUR);
        OPCODE_NAME(NOP);
        OPCODE_NAME(UNARY_POSITIVE);
        OPCODE_NAME(UNARY_NEGATIVE);
        OPCODE_NAME(UNARY_NOT);
        OPCODE_NAME(UNARY_INVERT);
        OPCODE_NAME(BINARY_MATRIX_MULTIPLY);
        OPCODE_NAME(INPLACE_MATRIX_MULTIPLY);
        OPCODE_NAME(BINARY_POWER);
        OPCODE_NAME(BINARY_MULTIPLY);
        OPCODE_NAME(BINARY_MODULO);
        OPCODE_NAME(BINARY_ADD);
        OPCODE_NAME(BINARY_SUBTRACT);
        OPCODE_NAME(BINARY_SUBSCR);
        OPCODE_NAME(BINARY_FLOOR_DIVIDE);
        OPCODE_NAME(BINARY_TRUE_DIVIDE);
        OPCODE_NAME(INPLACE_FLOOR_DIVIDE);
        OPCODE_NAME(INPLACE_TRUE_DIVIDE);
        OPCODE_NAME(GET_AITER);
        OPCODE_NAME(GET_ANEXT);
        OPCODE_NAME(BEFORE_ASYNC_WITH);
        OPCODE_NAME(BEGIN_FINALLY);
        OPCODE_NAME(END_ASYNC_FOR);
        OPCODE_NAME(INPLACE_ADD);
        OPCODE_NAME(INPLACE_SUBTRACT);
        OPCODE_NAME(INPLACE_MULTIPLY);
        OPCODE_NAME(INPLACE_MODULO);
        OPCODE_NAME(STORE_SUBSCR);
        OPCODE_NAME(DELETE_SUBSCR);
        OPCODE_NAME(BINARY_LSHIFT);
        OPCODE_NAME(BINARY_RSHIFT);
        OPCODE_NAME(BINARY_AND);
        OPCODE_NAME(BINARY_XOR);
        OPCODE_NAME(BINARY_OR);
        OPCODE_NAME(INPLACE_POWER);
        OPCODE_NAME(GET_ITER);
        OPCODE_NAME(GET_YIELD_FROM_ITER);
        OPCODE_NAME(PRINT_EXPR);
        OPCODE_NAME(LOAD_BUILD_CLASS);
        OPCODE_NAME(YIELD_FROM);
        OPCODE_NAME(GET_AWAITABLE);
        OPCODE_NAME(INPLACE_LSHIFT);
        OPCODE_NAME(INPLACE_RSHIFT);
        OPCODE_NAME(INPLACE_AND);
        OPCODE_NAME(INPLACE_XOR);
        OPCODE_NAME(INPLACE_OR);
        OPCODE_NAME(WITH_CLEANUP_START);
        OPCODE_NAME(WITH_CLEANUP_FINISH);
        OPCODE_NAME(RETURN_VALUE);
        OPCODE_NAME(IMPORT_STAR);
        OPCODE_NAME(SETUP_ANNOTATIONS);
        OPCODE_NAME(YIELD_VALUE);
        OPCODE_NAME(POP_BLOCK);
        OPCODE_NAME(END_FINALLY);
        OPCODE_NAME(POP_EXCEPT);
        OPCODE_NAME(STORE_NAME);
        OPCODE_NAME(DELETE_NAME);
        OPCODE_NAME(UNPACK_SEQUENCE);
        OPCODE_NAME(FOR_ITER);
        OPCODE_NAME(UNPACK_EX);
        OPCODE_NAME(STORE_ATTR);
        OPCODE_NAME(DELETE_ATTR);
        OPCODE_NAME(STORE_GLOBAL);
        OPCODE_NAME(DELETE_GLOBAL);
        OPCODE_NAME(LOAD_CONST);
        OPCODE_NAME(LOAD_NAME);
        OPCODE_NAME(BUILD_TUPLE);
        OPCODE_NAME(BUILD_LIST);
        OPCODE_NAME(BUILD_SET);
        OPCODE_NAME(BUILD_MAP);
        OPCODE_NAME(LOAD_ATTR);
        OPCODE_NAME(COMPARE_OP);
        OPCODE_NAME(IMPORT_NAME);
        OPCODE_NAME(IMPORT_FROM);
        OPCODE_NAME(JUMP_FORWARD);
        OPCODE_NAME(JUMP_IF_FALSE_OR_POP);
        OPCODE_NAME(JUMP_IF_TRUE_OR_POP);
        OPCODE_NAME(JUMP_ABSOLUTE);
        OPCODE_NAME(POP_JUMP_IF_FALSE);
        OPCODE_NAME(POP_JUMP_IF_TRUE);
        OPCODE_NAME(LOAD_GLOBAL);
        OPCODE_NAME(SETUP_FINALLY);
        OPCODE_NAME(LOAD_FAST);
        OPCODE_NAME(STORE_FAST);
        OPCODE_NAME(DELETE_FAST);
        OPCODE_NAME(RAISE_VARARGS);
        OPCODE_NAME(CALL_FUNCTION);
        OPCODE_NAME(MAKE_FUNCTION);
        OPCODE_NAME(BUILD_SLICE);
        OPCODE_NAME(LOAD_CLOSURE);
        OPCODE_NAME(LOAD_DEREF);
        OPCODE_NAME(STORE_DEREF);
        OPCODE_NAME(DELETE_DEREF);
        OPCODE_NAME(CALL_FUNCTION_KW);
        OPCODE_NAME(CALL_FUNCTION_EX);
        OPCODE_NAME(SETUP_WITH);
        OPCODE_NAME(EXTENDED_ARG);
        OPCODE_NAME(LIST_APPEND);
        OPCODE_NAME(SET_ADD);
        OPCODE_NAME(MAP_ADD);
        OPCODE_NAME(LOAD_CLASSDEREF);
        OPCODE_NAME(BUILD_LIST_UNPACK);
        OPCODE_NAME(BUILD_MAP_UNPACK);
        OPCODE_NAME(BUILD_MAP_UNPACK_WITH_CALL);
        OPCODE_NAME(BUILD_TUPLE_UNPACK);
        OPCODE_NAME(BUILD_SET_UNPACK);
        OPCODE_NAME(SETUP_ASYNC_WITH);
        OPCODE_NAME(FORMAT_VALUE);
        OPCODE_NAME(BUILD_CONST_KEY_MAP);
        OPCODE_NAME(BUILD_STRING);
        OPCODE_NAME(BUILD_TUPLE_UNPACK_WITH_CALL);
        OPCODE_NAME(LOAD_METHOD);
        OPCODE_NAME(CALL_METHOD);
        OPCODE_NAME(CALL_FINALLY);
        OPCODE_NAME(POP_FINALLY);
    };
#undef OPCODE_NAME
    return "UNKNOWN";
}

#define IS_32BIT_VAL(x) ((unsigned long)(x) <= UINT32_MAX)
#define IS_32BIT_SIGNED_VAL(x) ((int32_t)(x) == (x))


// result must be freed()
static char* calculate_jmp_targets(const _Py_CODEUNIT *first_instr, const int num_opcodes) {
    // TODO: could be a bit vector
    char* is_jmp_target = malloc(num_opcodes);
    memset(is_jmp_target, 0, num_opcodes);
    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx < num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = first_instr[inst_idx];
        int opcode = _Py_OPCODE(word);
        int oparg = _Py_OPARG(word);
        // this is used for the special EXTENDED_ARG opcode
        oparg |= oldoparg;
        oldoparg = 0;

        switch (opcode) {
            case JUMP_ABSOLUTE:
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
                is_jmp_target[oparg/2] = 1;
                break;

            case JUMP_FORWARD:
            case FOR_ITER:
                is_jmp_target[oparg/2 + inst_idx + 1] = 1;
                break;

            case CALL_FINALLY:
                is_jmp_target[inst_idx + 1] = 1;
                is_jmp_target[oparg/2 + inst_idx + 1] = 1;
                break;

            // this opcodes use PyFrame_BlockSetup which is similar to a jump in case of exception
            case SETUP_ASYNC_WITH:
            case SETUP_FINALLY:
            case SETUP_WITH:
                is_jmp_target[inst_idx + 1] = 1;
                is_jmp_target[oparg/2 + inst_idx + 1] = 1;
                break;

            case YIELD_FROM:
            case YIELD_VALUE:
                is_jmp_target[inst_idx + 0] = 1;
                is_jmp_target[inst_idx + 1] = 1;
                break;

            case EXTENDED_ARG:
                oldoparg = oparg << 8;
                break;
        }
    }
    return is_jmp_target;
}

// returns if any of the functions arguments get deleted (checks for DELETE_FAST)
static int check_func_args_never_deleted(const _Py_CODEUNIT *first_instr, const int num_opcodes, const int num_args) {
    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx < num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = first_instr[inst_idx];
        int opcode = _Py_OPCODE(word);
        int oparg = _Py_OPARG(word);
        // this is used for the special EXTENDED_ARG opcode
        oparg |= oldoparg;
        oldoparg = 0;

        switch (opcode) {
            case DELETE_FAST:
                if (oparg < num_args)
                    return 0; // this is deleting a function arg!
                break;

            case EXTENDED_ARG:
                oldoparg = oparg << 8;
                break;
        }
    }
    return 1;
}

static int8_t* mem_chunk = NULL;
static size_t mem_chunk_bytes_remaining = 0;
static long mem_bytes_allocated = 0, mem_bytes_used = 0;
static long mem_bytes_used_max = 100*1000*1000; // will stop emitting code after that many bytes

static int jit_stats_enabled = 0;
static unsigned long jit_stat_load_attr_hit, jit_stat_load_attr_miss, jit_stat_load_attr_inline, jit_stat_load_attr_total;
static unsigned long jit_stat_load_method_hit, jit_stat_load_method_miss, jit_stat_load_method_inline, jit_stat_load_method_total;
static unsigned long jit_stat_load_global_hit, jit_stat_load_global_miss, jit_stat_load_global_inline, jit_stat_load_global_total;

#define ENABLE_DEFERRED_LOAD_CONST 1
#define ENABLE_DEFERRED_LOAD_FAST 1
#define ENABLE_DEFERRED_RES_PUSH 1
#define ENABLE_DEFINED_TRACKING 1
#define ENABLE_AVOID_SIG_TRACE_CHECK 1


|.arch x64
|.section entry, code, cold, opcode_addr

////////////////////////////////
// REGISTER DEFINITIONS

// all this values are in callee saved registers
// NOTE: r13 and rbp need 1 byte more to encode a direct memory accesss without offset
// e.g. mov rax, [rbp] is encoded as mov rax, [rbp + 0]
|.define f, r13
|.define preserved_reg, rbp // this register gets used when we have to make a call but preserve a value
#define preserved_reg_idx 5
|.define preserved_reg2, r14 // this register gets used by deferred_vs when we have to make a call but preserve a value
#define preserved_reg2_idx 14
|.define tstate, r15
|.define vsp, r12
|.define interrupt, rbx // if you change this you may have to adjust jmp_to_inst_idx
#define interrupt_idx 3

// follow AMD64 calling convention
// instruction indices can be found here: https://corsix.github.io/dynasm-doc/instructions.html
|.define arg1, rdi
#define arg1_idx 7
|.define arg2, rsi
#define arg2_idx 6
|.define arg3, rdx
#define arg3_idx 2
|.define arg4, rcx
#define arg4_idx 1
|.define arg5, r8
#define arg5_idx 8
|.define arg6, r9
#define arg6_idx 9
// return values
|.define res, rax
#define res_idx 0
|.define res2, rdx
|.define res_32b, eax

// will be used by macros
|.define tmp, r9
#define tmp_idx 9

static void deferred_vs_apply(Jit* Dst);

static void switch_section(Jit* Dst, Section new_section) {
    Dst->current_section = new_section;
    if (new_section == SECTION_CODE) {
        |.code
    } else if (new_section == SECTION_COLD) {
        |.cold
    } else if (new_section == SECTION_ENTRY) {
        |.entry
    } else if (new_section == SECTION_OPCODE_ADDR) {
        |.opcode_addr
    } else {
        printf("unknown section");
        abort();
    }
}

// moves the value stack pointer by num_values python objects
static void emit_adjust_vs(Jit* Dst, int num_values) {
    if (num_values > 0) {
        | add vsp, 8*num_values
    } else if (num_values < 0) {
        | sub vsp, 8*-num_values
    }
}

static void emit_push_v(Jit* Dst, int r_idx) {
    deferred_vs_apply(Dst);
    | mov [vsp], Rq(r_idx)
    emit_adjust_vs(Dst, 1);
}

static void emit_pop_v(Jit* Dst, int r_idx) {
    deferred_vs_apply(Dst);
    emit_adjust_vs(Dst, -1);
    | mov Rq(r_idx), [vsp]
}

// top = 1, second = 2, third = 3,...
static void emit_read_vs(Jit* Dst, int r_idx, int stack_offset) {
    deferred_vs_apply(Dst);
    | mov Rq(r_idx), [vsp - (8*stack_offset)]
}

static void emit_write_vs(Jit* Dst, int r_idx, int stack_offset) {
    deferred_vs_apply(Dst);
    | mov [vsp - (8*stack_offset)], Rq(r_idx)
}

static void emit_incref(Jit* Dst, int r_idx) {
    _Static_assert(offsetof(PyObject, ob_refcnt) == 0,  "add needs to be modified");
#ifdef Py_REF_DEBUG
    _Static_assert(sizeof(_Py_RefTotal) == 8,  "adjust inc qword");
    | inc qword [&_Py_RefTotal]
#endif
    | inc qword [Rq(r_idx)]
}

static void emit_if_res_0_error(Jit* Dst) {
    | test res, res
    | jz ->error
}

static void emit_if_res_32b_not_0_error(Jit* Dst) {
    | test res_32b, res_32b
    | jnz ->error
}

static void emit_jump_by_n_bytecodes(Jit* Dst, int num_bytes, int inst_idx) {
    | jmp => ((num_bytes)/2+inst_idx+1)
}

static void emit_jump_to_bytecode_n(Jit* Dst, int num_bytes) {
    | jmp => (num_bytes)/2
}

static void emit_je_to_bytecode_n(Jit* Dst, int num_bytes) {
    | je => (num_bytes)/2
}

// moves a 32bit or 64bit immediate into a register uses smallest encoding
static void emit_mov_imm(Jit* Dst, int r_idx, unsigned long val) {
    if (val == 0) {
        | xor Rd(r_idx), Rd(r_idx)
    } else if (IS_32BIT_VAL(val)) {
        | mov Rd(r_idx), (unsigned int)val
    } else {
        | mov64 Rq(r_idx), (unsigned long)val
    }
}

static void emit_cmp_imm(Jit* Dst, int r_idx, unsigned long val) {
    if (IS_32BIT_VAL(val)) {
        | cmp Rq(r_idx), (unsigned int)val
    } else {
        | mov64 tmp, (unsigned long)val
        | cmp Rq(r_idx), tmp
    }
}

static void emit_qword(Jit* Dst, unsigned long val_orig) {
    for (unsigned long val = val_orig, i=0; i<8; ++i, val >>= 8) {
        | .byte val & 0xFF
    }
}


// Loads a register `r_idx` with a value `addr`, potentially doing a lea
// off of another register `other_idx` which contains a known value `other_addr`
static void emit_mov_imm_or_lea(Jit* Dst, int r_idx, int other_idx, void* addr, void* other_addr) {
    ptrdiff_t diff = (uintptr_t)addr - (uintptr_t)other_addr;
    if (!IS_32BIT_SIGNED_VAL((unsigned long)addr) && IS_32BIT_SIGNED_VAL(diff)) {
        | lea Rq(r_idx), [Rq(other_idx)+diff]
    } else {
        emit_mov_imm(Dst, r_idx, (unsigned long)addr);
    }
}

// sets register r_idx1 = addr1 and r_idx2 = addr2. Uses a lea if beneficial.
static void emit_mov_imm2(Jit* Dst, int r_idx1, void* addr1, int r_idx2, void* addr2) {
    emit_mov_imm(Dst, r_idx1, addr1);
    emit_mov_imm_or_lea(Dst, r_idx2, r_idx1, addr2, addr1);
}

static void emit_call_ext_func(Jit* Dst, void* addr) {
    if (IS_32BIT_SIGNED_VAL((long)addr)) {
        // This emits a relative call. The dynasm syntax is confusing
        // it will not actually take the address of addr (even thoug it says &addr).
        // Put instead just take the value of addr and calculate the difference to the emitted instruction address. (generated code: dasm_put(Dst, 135, (ptrdiff_t)(addr)))
        | call qword &addr
    } else {
        emit_mov_imm(Dst, res_idx, (unsigned long)addr);
        | call res
    }
}

// r_idx contains the PyObject to decref
// Note: this macro clobbers all registers except 'res' if preserve_res is set
// Can't use label 9 here because it will end up being the target
// of xdecref's jump
// it's best to decref arg1 because it uses one less mov instruction
static void emit_decref(Jit* Dst, int r_idx, int preserve_res) {
    _Static_assert(offsetof(PyObject, ob_refcnt) == 0,  "sub needs to be modified");
#ifdef Py_REF_DEBUG
    | dec qword [&_Py_RefTotal]
#endif
    | dec qword [Rq(r_idx)]

    // normally we emit the dealloc call into the cold section but if we are already inside it
    // we have to instead emit it inline
    int use_inline_decref = Dst->current_section == SECTION_COLD;
    if (use_inline_decref) {
        | jnz >8
    } else {
        | jz >8
        switch_section(Dst, SECTION_COLD);
        |8:
    }

    if (r_idx != arg1_idx) { // setup the call argument
        | mov Rq(arg1_idx), Rq(r_idx)
    }
    if (preserve_res) {
        | mov preserved_reg, res // save the result
    }

    // inline _Py_Dealloc
    //  call_ext_func _Py_Dealloc
    | mov res, [arg1 + offsetof(PyObject, ob_type)]
    | call qword [res + offsetof(PyTypeObject, tp_dealloc)]

    if (preserve_res) {
        | mov res, preserved_reg
    }

    if (use_inline_decref) {
        |8:
    } else {
        | jmp >8
        switch_section(Dst, SECTION_CODE);
        |8:
    }
}

static void emit_decref_stored_args(Jit* Dst, int num, RefStatus ref_status[]) {
    // if no deferred vs entry is using preserved_reg2 we can use it because we don't generate new
    // deferred vs entries.
    // Priorities for storage location are:
    //   - preserved_reg
    //   - preserved_reg2 - if available
    //   - stack slot
    int can_use_preserved_reg2 = Dst->deferred_vs_reg2_used == 0;
    for (int i=0, num_decref=0; i<num; ++i) {
        if (ref_status[i] != OWNED)
            continue;
        if (num_decref == 0) {
            emit_decref(Dst, preserved_reg_idx, 1); /*= preserve res */
        } else if (num_decref == 1 && can_use_preserved_reg2) {
            emit_decref(Dst, preserved_reg2_idx, 1); /*= preserve res */
            emit_mov_imm(Dst, preserved_reg2_idx, 0); // we have to clear it because error path will xdecref
        } else {
            int stack_slot = num_decref - can_use_preserved_reg2 -1;
            // this should never happen if it does adjust NUM_MANUAL_STACK_SLOTS
            if (stack_slot >= NUM_MANUAL_STACK_SLOTS)
                abort();
            | mov arg1, [rsp + stack_slot*8]
            emit_decref(Dst, arg1_idx, 1); /*= preserve res */
        }
        ++num_decref;
    }
}


static void emit_decref_cant_0(Jit* Dst, int r_idx) {
    _Static_assert(offsetof(PyObject, ob_refcnt) == 0,  "sub needs to be modified");
#ifdef Py_REF_DEBUG
    | dec qword [&_Py_RefTotal]
#endif
    | dec qword [Rq(r_idx)]
}

static void emit_xdecref_arg1(Jit* Dst) {
    | test arg1, arg1
    | jz >9
    emit_decref(Dst, arg1_idx, 0 /* don't preserve res */);
    |9:
}

static void emit_store_decref_args(Jit* Dst, int num, int regs[], RefStatus ref_status[]) {
    int num_owned = 0;
    int can_use_preserved_reg2 = Dst->deferred_vs_reg2_used == 0;
    for (int i=0; i<num; ++i) {
        if (ref_status[i] != OWNED)
            continue;
        if (num_owned == 0) {
            | mov preserved_reg, Rq(regs[i])
        } else if (num_owned == 1 && can_use_preserved_reg2) {
            | mov preserved_reg2, Rq(regs[i])
        } else {
            int stack_slot = num_owned - can_use_preserved_reg2 -1;
            | mov [rsp + stack_slot*8], Rq(regs[i])
        }
        ++num_owned;
    }
}
static void emit_store_decref_args1(Jit* Dst, int r1_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx };
    emit_store_decref_args(Dst, 1, regs, ref_status);
}
static void emit_store_decref_args2(Jit* Dst, int r1_idx, int r2_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx, r2_idx };
    emit_store_decref_args(Dst, 2, regs, ref_status);
}
static void emit_store_decref_args3(Jit* Dst, int r1_idx, int r2_idx, int r3_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx, r2_idx, r3_idx };
    emit_store_decref_args(Dst, 3, regs, ref_status);
}

static void emit_aot_func_call(Jit* Dst, int num_args, int opcode, int oparg, int opcache_available) {
    // We emit a relative call to the destination function here which can be patched.
    // The address of the call is retrieved via __builtin_return_address(0) inside the AOT func and
    // then the destination of the call instruction (=relative address of the function to call) is modified.
    unsigned long addr = (unsigned long)get_addr_of_aot_func(opcode, oparg, opcache_available);
    if (!IS_32BIT_VAL(addr)) {
        printf("aborting func addr does not fit into 32bit.\n");
        abort();
    }
    // this will generate a 5 byte long relative call.
    // 0xe8 <4 byte rel offset>
    | call qword &addr
}

static void emit_jmp_to_inst_idx(Jit* Dst, int r_idx, int skip_sig_check) {
    if (r_idx == tmp_idx)
        abort(); // "can't be tmp"

    | lea tmp, [->opcode_addr_begin]
    // *2 instead of *4 because:
    // entries are 4byte wide addresses but lasti needs to be divided by 2
    // because it tracks offset in bytecode (2bytes long) array not the index
    | mov Rd(tmp_idx), [tmp + Rq(r_idx)*2]
    if (skip_sig_check) {
        // tmp points now to the beginning of the bytecode implementation
        // but we want to skip the signal+tracing check.
        // We can't just directly jump after the signal check beause the jne instruction is variable size
        // so instead jump before the conditional jump and set the flags so that we don't jump

        // size of 'mov dword [lasti + offsetof(PyFrameObject, f_lasti)], inst_idx*2' + 'cmp qword [interrupt], 0'
        | add tmp, 8 + 4
        | cmp tmp, tmp // dummy to set the flags
    }
    | jmp tmp
}

// warning this will overwrite tmp and arg1
static void emit_jmp_to_lasti(Jit* Dst, int skip_sig_check) {
    | mov Rd(arg1_idx), [f + offsetof(PyFrameObject, f_lasti)]
    emit_jmp_to_inst_idx(Dst, arg1_idx, skip_sig_check);
}

static int get_fastlocal_offset(int fastlocal_idx) {
    return offsetof(PyFrameObject, f_localsplus) + fastlocal_idx * 8;
}

// this does the same as: r = freevars[num]
static void emit_load_freevar(Jit* Dst, int r_idx, int num) {
    // PyObject *cell = (f->f_localsplus + co->co_nlocals)[oparg];
    | mov Rq(r_idx), [f + get_fastlocal_offset(Dst->co->co_nlocals + num)]
}

//////////////////////////////////////////////////////////////
// Deferred value stack functions
static void deferred_vs_emit(Jit* Dst) {
    if (Dst->deferred_vs_next) {
        for (int i=Dst->deferred_vs_next; i>0; --i) {
            if (Dst->deferred_vs[i-1].loc == CONST) {
                PyObject* obj = PyTuple_GET_ITEM(Dst->co_consts, Dst->deferred_vs[i-1].val);
                emit_mov_imm(Dst, tmp_idx, (unsigned long)obj);
                if (!is_immortal(obj))
                    emit_incref(Dst, tmp_idx);
                | mov [vsp+ 8 * (i-1)], tmp
            } else if (Dst->deferred_vs[i-1].loc == FAST) {
                | mov tmp, [f + get_fastlocal_offset(Dst->deferred_vs[i-1].val)]
                emit_incref(Dst, tmp_idx);
                | mov [vsp+ 8 * (i-1)], tmp
            } else if (Dst->deferred_vs[i-1].loc == REGISTER) {
                | mov [vsp+ 8 * (i-1)], Rq(Dst->deferred_vs[i-1].val)
                if (Dst->deferred_vs[i-1].val == preserved_reg2_idx) {
                    emit_mov_imm(Dst, preserved_reg2_idx, 0); // we have to clear it because error path will xdecref
                }
            } else if (Dst->deferred_vs[i-1].loc == STACK) {
                | mov tmp, [rsp + (Dst->deferred_vs[i-1].val + NUM_MANUAL_STACK_SLOTS) * 8]
                | mov qword [rsp + (Dst->deferred_vs[i-1].val + NUM_MANUAL_STACK_SLOTS) * 8], 0
                | mov [vsp+ 8 * (i-1)], tmp
            } else {
                abort();
            }
        }
        emit_adjust_vs(Dst, Dst->deferred_vs_next);
    }
}

// if there are any deferred python value stack operations they will be emitted
// and the value stack variables are reset
static void deferred_vs_apply(Jit* Dst) {
    if (Dst->deferred_vs_next) {
        deferred_vs_emit(Dst);
        Dst->deferred_vs_next = 0;
        Dst->deferred_stack_slot_next = 0;
        Dst->deferred_vs_reg2_used = 0;
    }
}

static void deferred_vs_push(Jit* Dst, int location, unsigned long value) {
    if (location == REGISTER && value == res_idx && !(ENABLE_DEFERRED_RES_PUSH)) {
        emit_push_v(Dst, res_idx);
    } else {
        if (Dst->deferred_vs_next + 1 >= DEFERRED_VS_MAX) { // make sure we are not writing out of bounds
            deferred_vs_apply(Dst); // TODO: we could just materialize the first stack item instead of all
        }
        Dst->deferred_vs[Dst->deferred_vs_next].loc = location;
        Dst->deferred_vs[Dst->deferred_vs_next].val = value;
        ++Dst->deferred_vs_next;
    }
}

// returns one of OWNED, BORROWED, or IMMORTAL based on the reference ownership status
static int deferred_vs_peek(Jit* Dst, int r_idx, int num) {
    RefStatus ref_status = OWNED;
    if (Dst->deferred_vs_next >= num) {
        int idx = Dst->deferred_vs_next-(num);
        if (Dst->deferred_vs[idx].loc == CONST) {
            PyObject* obj = PyTuple_GET_ITEM(Dst->co_consts, Dst->deferred_vs[idx].val);
            emit_mov_imm(Dst, r_idx, (unsigned long)obj);
            ref_status = is_immortal(obj) ? IMMORTAL : BORROWED;
        } else if (Dst->deferred_vs[idx].loc == FAST) {
            | mov Rq(r_idx), [f + get_fastlocal_offset(Dst->deferred_vs[idx].val)]
            ref_status = BORROWED;
        } else if (Dst->deferred_vs[idx].loc == REGISTER) {
            // only generate mov if src and dst is different
            if (r_idx != Dst->deferred_vs[idx].val) {
                | mov Rq(r_idx), Rq(Dst->deferred_vs[idx].val)
            }
            ref_status = OWNED;
        } else if (Dst->deferred_vs[idx].loc == STACK) {
            | mov Rq(r_idx), [rsp + (Dst->deferred_vs[idx].val + NUM_MANUAL_STACK_SLOTS) * 8]
            ref_status = OWNED;
        } else {
            abort();
        }
    } else {
        | mov Rq(r_idx), [vsp-8*((num)-Dst->deferred_vs_next)]
        ref_status = OWNED;
    }
    return ref_status;
}

static void deferred_vs_peek_owned(Jit* Dst, int r_idx, int num) {
    RefStatus ref_status = deferred_vs_peek(Dst, r_idx, num);
    if (ref_status == BORROWED) {
        emit_incref(Dst, r_idx);
    }
}

static void deferred_vs_convert_reg_to_stack(Jit* Dst) {
    for (int i=Dst->deferred_vs_next; i>0; --i) {
        if (Dst->deferred_vs[i-1].loc != REGISTER)
            continue;
        // we only need to handle register 'res' because 'preserved_reg2' will not
        // get overwritten by a call.
        if (Dst->deferred_vs[i-1].val != res_idx)
            continue;

        // if we have 'preserved_reg2' available use it over a stack slot
        if (!Dst->deferred_vs_reg2_used) {
            | mov preserved_reg2, Rq(Dst->deferred_vs[i-1].val)
            Dst->deferred_vs[i-1].loc = REGISTER;
            Dst->deferred_vs[i-1].val = preserved_reg2_idx;
            Dst->deferred_vs_reg2_used = 1;
            continue;
        }

        if (Dst->num_deferred_stack_slots <= Dst->deferred_stack_slot_next)
            ++Dst->num_deferred_stack_slots;
        | mov [rsp + (Dst->deferred_stack_slot_next + NUM_MANUAL_STACK_SLOTS) * 8], res
        Dst->deferred_vs[i-1].loc = STACK;
        Dst->deferred_vs[i-1].val = Dst->deferred_stack_slot_next;
        ++Dst->deferred_stack_slot_next;
    }
}

// removes the top num elements from the value stack
static void deferred_vs_remove(Jit* Dst, int num_to_remove) {
    if (!num_to_remove)
        return;

    for (int i=0; i < num_to_remove && Dst->deferred_vs_next > i; ++i) {
        if (Dst->deferred_vs[Dst->deferred_vs_next-1-i].loc == STACK) {
            | mov qword [rsp + (Dst->deferred_vs[Dst->deferred_vs_next-1-i].val + NUM_MANUAL_STACK_SLOTS) * 8], 0
            if (Dst->deferred_stack_slot_next-1 == Dst->deferred_vs[Dst->deferred_vs_next-1-i].val)
                --Dst->deferred_stack_slot_next;
        } else if (Dst->deferred_vs[Dst->deferred_vs_next-1-i].loc == REGISTER) {
            if (Dst->deferred_vs[Dst->deferred_vs_next-1-i].val == preserved_reg2_idx) {
                emit_mov_imm(Dst, preserved_reg2_idx, 0); // we have to clear it because error path will xdecref
                Dst->deferred_vs_reg2_used = 0;
            }
        }
    }
    if (Dst->deferred_vs_next >= num_to_remove) {
        Dst->deferred_vs_next -= num_to_remove;
    } else {
        emit_adjust_vs(Dst, -(num_to_remove - Dst->deferred_vs_next));
        Dst->deferred_vs_next = 0;
    }
}

// peeks the top stack entry into register r_idx_top and afterwards calls deferred_vs_apply
// generates better code than doing it split
static void deferred_vs_peek_top_and_apply(Jit* Dst, int r_idx_top) {
    assert(r_idx_top != preserved_reg2_idx);
    if (Dst->deferred_vs_next) {
        // load the value into the destination register and replace the deferred_vs entry
        // with one which accesses the register instead.
        // This is safe because we erase all deferred_vs entries afterwards in deferred_vs_apply.
        // so no additional code needs to know about this register use.
        // Without this we would e.g. generate two memory loads if the top entry is a FAST var access.

        // owned because deferred_vs_apply will consume one ref
        deferred_vs_peek_owned(Dst, r_idx_top, 1 /*=top*/);
        deferred_vs_remove(Dst, 1);
        deferred_vs_push(Dst, REGISTER, r_idx_top);
        deferred_vs_apply(Dst);
    } else {
        deferred_vs_peek(Dst, r_idx_top, 1 /*=top*/);
    }
}

static void deferred_vs_pop_n(Jit* Dst, int num, const int* const regs, int out_ref_status[]) {
    for (int i=0; i<num; ++i) {
        out_ref_status[i] = deferred_vs_peek(Dst, regs[i], i+1);
    }
    deferred_vs_remove(Dst, num);
}
// returns one of BORROWED, OWNED, or IMMORTAL
static int deferred_vs_pop1(Jit* Dst, int r_idx1) {
    int regs[] = { r_idx1 };
    RefStatus ref_status;
    deferred_vs_pop_n(Dst, 1, regs, &ref_status);
    return ref_status;
}
static void deferred_vs_pop2(Jit* Dst, int r_idx1, int r_idx2, int out_ref_status[]) {
    int regs[] = { r_idx1, r_idx2 };
    deferred_vs_pop_n(Dst, 2, regs, out_ref_status);
}
static void deferred_vs_pop3(Jit* Dst, int r_idx1, int r_idx2, int r_idx3, int out_ref_status[]) {
    int regs[] = { r_idx1, r_idx2, r_idx3 };
    deferred_vs_pop_n(Dst, 3, regs, out_ref_status);
}

static void deferred_vs_pop_n_owned(Jit* Dst, int num, const int* const regs) {
    RefStatus ref_status[num];
    deferred_vs_pop_n(Dst, num, regs, ref_status);
    for (int i=0; i<num; ++i) {
        if (ref_status[i] == BORROWED) {
            emit_incref(Dst, regs[i]);
        }
    }
}
static void deferred_vs_pop1_owned(Jit* Dst, int r_idx1) {
    int regs[] = { r_idx1 };
    deferred_vs_pop_n_owned(Dst, 1, regs);
}
static void deferred_vs_pop2_owned(Jit* Dst, int r_idx1, int r_idx2) {
    int regs[] = { r_idx1, r_idx2 };
    deferred_vs_pop_n_owned(Dst, 2, regs);
}
static void deferred_vs_pop3_owned(Jit* Dst, int r_idx1, int r_idx2, int r_idx3) {
    int regs[] = { r_idx1, r_idx2, r_idx3 };
    deferred_vs_pop_n_owned(Dst, 3, regs);
}

// if the same variable is inside our deferred value array we have to create the stack
static void deferred_vs_apply_if_same_var(Jit* Dst, int var_idx) {
    int materialize_stack = 0;
    for (int i=Dst->deferred_vs_next; i>0; --i) {
        if (Dst->deferred_vs[i-1].loc == FAST && Dst->deferred_vs[i-1].val == var_idx) {
            materialize_stack = 1;
            break;
        }
    }
    if (materialize_stack) {
        deferred_vs_apply(Dst);
    } else {
        deferred_vs_convert_reg_to_stack(Dst);
    }
}

// returns 0 if IC generation succeeded
static int emit_inline_cache(Jit* Dst, int opcode, int oparg, _PyOpcache* co_opcache) {
    // MACROS TO MAKE ASM LIFE EASIER

    // Same as cmp_imm, but if r is a memory expression we need to specify the size of the load.
    |.macro cmp_imm_mem, r, addr
    || if (IS_32BIT_VAL(addr)) {
    |       cmp qword r, (unsigned int)addr
    || } else {
    |       mov64 tmp, (unsigned long)addr
    |       cmp r, tmp
    || }
    |.endmacro

    // checks if type object in r_type has the valid version tag set and compares tp_version_tag with type_ver
    // branches to false_branch on inequality else continues
    |.macro type_version_check, r_type, type_ver, false_branch
    || _Static_assert(Py_TPFLAGS_VALID_VERSION_TAG == (1UL << 19),  "test needs to be modified");
    || // !PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_VALID_VERSION_TAG)
    |  test byte [r_type + offsetof(PyTypeObject, tp_flags) + 2], 8
    |  je false_branch
    || // Py_TYPE(obj)->tp_version_tag == type_ver
    |  cmp_imm_mem [r_type + offsetof(PyTypeObject, tp_version_tag)], type_ver
    |  jne false_branch
    |.endmacro

    if (opcode == LOAD_GLOBAL)  {
        ++jit_stat_load_global_total;
        // The co_opcache->num_failed==0 check is to try to avoid writing out inline
        // caches that might end up missing, since we currently don't rewrite them.
        // It looks like the check is largely useless on our benchmarks, and doesn't
        // meaningfully cut down on the (extremely small) number of cache misses.
        // I think it's still worth leaving it in to reduce potential downside in bad cases,
        // as it definitely helps with the other opcodes.
        // globals_ver != 0 makes sure we don't write out an always-failing inline cache
        if (co_opcache != NULL && co_opcache->num_failed == 0 && co_opcache->u.lg.globals_ver != 0) {
            _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

            ++jit_stat_load_global_inline;

            deferred_vs_convert_reg_to_stack(Dst);

            | mov arg3, [f + offsetof(PyFrameObject, f_globals)]
            | cmp_imm_mem [arg3 + offsetof(PyDictObject, ma_version_tag)], lg->globals_ver
            | jne >1
            | mov arg3, [f + offsetof(PyFrameObject, f_builtins)]
            | cmp_imm_mem [arg3 + offsetof(PyDictObject, ma_version_tag)], lg->builtins_ver
            | jne >1
            emit_mov_imm(Dst, res_idx, (unsigned long)lg->ptr);
            emit_incref(Dst, res_idx);
            if (jit_stats_enabled) {
                | inc qword [&jit_stat_load_global_hit]
            }
            |4:
            deferred_vs_push(Dst, REGISTER, res_idx);
            // fallthrough to next opcode

            // Put the slowpath in a cold section
            switch_section(Dst, SECTION_COLD);
            | 1:
            if (jit_stats_enabled) {
                | inc qword [&jit_stat_load_global_miss]
            }
            emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                arg2_idx, co_opcache);
            emit_aot_func_call(Dst, 2, opcode, oparg, co_opcache != 0 /*= use op cache */);
            emit_if_res_0_error(Dst);
            | jmp <4 // jump to the common code which pushes the result
            // Switch back to the normal section
            switch_section(Dst, SECTION_CODE);
            return 0;
        }
    } else if (opcode == LOAD_ATTR || opcode == LOAD_METHOD) {
        if (opcode == LOAD_ATTR)
            ++jit_stat_load_attr_total;
        else
            ++jit_stat_load_method_total;
        if (co_opcache != NULL && co_opcache->num_failed == 0 && co_opcache->u.la.type_ver != 0) {
            if (opcode == LOAD_ATTR)
                ++jit_stat_load_attr_inline;
            else
                ++jit_stat_load_method_inline;

            _PyOpcache_LoadAttr *la = &co_opcache->u.la;

            int emit_load_attr_res_0_helper = 0;

            int version_zero = (la->cache_type == LA_CACHE_VALUE_CACHE_DICT && la->u.value_cache.dict_ver == 0) ||
                (la->cache_type == LA_CACHE_IDX_SPLIT_DICT && la->u.split_dict_cache.splitdict_keys_version == 0);

            if (version_zero == 1 && la->cache_type == LA_CACHE_IDX_SPLIT_DICT) {
                // This case is currently impossible since it will always be a miss and we don't cache
                // misses, so it's untested.
                fprintf(stderr, "untested jit case");
                abort();
            }

            // In comparison to the LOAD_METHOD cache, label 2 is when we know the version checks have
            // passed, but there's an additional tp_descr_get check that we have to do


            RefStatus ref_status = 0;
            if (opcode == LOAD_ATTR) {
                // PyObject *owner = POP();
                ref_status = deferred_vs_pop1(Dst, arg1_idx);
                deferred_vs_convert_reg_to_stack(Dst);
            } else {
                // PyObject *obj = TOP();
                deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            }

            // loadAttrCache
            // PyTypeObject *arg2 = Py_TYPE(obj)
            | mov arg2, [arg1 + offsetof(PyObject, ob_type)]
            | type_version_check, arg2, la->type_ver, >1

            if (la->cache_type == LA_CACHE_DATA_DESCR) {
                // save the obj so we can access it after the call
                | mov preserved_reg, arg1

                PyObject* descr = la->u.descr_cache.descr;
                emit_mov_imm(Dst, tmp_idx, (unsigned long)descr);
                | mov arg2, [tmp + offsetof(PyObject, ob_type)]
                | type_version_check, arg2, la->u.descr_cache.descr_type_ver, >1

                // res = descr->ob_type->tp_descr_get(descr, owner, (PyObject *)owner->ob_type);
                | mov arg1, tmp
                | mov arg2, preserved_reg
                | mov arg3, [preserved_reg + offsetof(PyObject, ob_type)]
                emit_call_ext_func(Dst, descr->ob_type->tp_descr_get);
                | mov arg1, preserved_reg // restore the obj so that the decref code works
                // attr can be NULL
                | test res, res
                | jz >3
                emit_load_attr_res_0_helper = 1; // makes sure we emit label 3
            } else {
                // _PyObject_GetDictPtr
                // arg2 = PyType(obj)->tp_dictoffset
                | mov arg2, [arg2 + offsetof(PyTypeObject, tp_dictoffset)]

                | test arg2, arg2
                // je -> tp_dictoffset == 0
                // tp_dict_offset==0 implies dict_ptr==NULL implies dict version (either split keys or not) is 0
                // Also, fail the cache if dictoffset<0 rather than do the lengthier dict_ptr computation
                // TODO some of the checks here might be redundant with the tp_version_tag check (specifies a class)
                // and the fact that we successfully wrote the cache the first time.
                if (version_zero) {
                    // offset==0 => automatic version check success
                    | je >2
                    // offset<0 => failure
                    | js >1
                } else {
                    // automatic failure if dict_offset is zero or if it is negative
                    | jle >1
                }

                // Now loadAttrCache splits into two cases, but the first step on both
                // is to load the dict pointer and check if it's null

                // arg2 = *(obj + dictoffset)
                | mov arg2, [arg1 + arg2]
                | test arg2, arg2
                if (version_zero) {
                    // null dict is always a cache hit
                    | jz >2
                } else {
                    // null dict is always a cache miss
                    | jz >1
                }
            }

            if (la->cache_type == LA_CACHE_OFFSET_CACHE)
            {
                // if (mp->ma_keys->dk_size != dk_size) goto slow_path;
                | mov res, [arg2 + offsetof(PyDictObject, ma_keys)]
                | cmp_imm_mem [res + offsetof(PyDictKeysObject, dk_size)], la->u.offset_cache.dk_size
                | jne >1

                // if (mp->ma_keys->dk_lookup == lookdict_split) goto slow_path;
                | cmp_imm_mem [res + offsetof(PyDictKeysObject, dk_lookup)], lookdict_split
                | je >1

                // PyDictKeyEntry *arg3 = (PyDictKeyEntry*)(mp->ma_keys->dk_indices + offset);
                uint64_t total_offset = offsetof(PyDictKeysObject, dk_indices) + la->u.offset_cache.offset;
                if (IS_32BIT_SIGNED_VAL(total_offset)) {
                    | lea arg3, [res + total_offset]
                } else {
                    emit_mov_imm(Dst, tmp_idx, total_offset);
                    | lea arg3, [res + tmp]
                }

                // if (ep->me_key != key) goto slow_path;
                | cmp_imm_mem [arg3 + offsetof(PyDictKeyEntry, me_key)], PyTuple_GET_ITEM(Dst->co_names, oparg)
                | jne >1

                // res = ep->me_value;
                | mov res, [arg3 + offsetof(PyDictKeyEntry, me_value)]
                emit_incref(Dst, res_idx);
            } else if (la->cache_type == LA_CACHE_VALUE_CACHE_DICT || la->cache_type == LA_CACHE_VALUE_CACHE_SPLIT_DICT) {
                if (la->cache_type == LA_CACHE_VALUE_CACHE_SPLIT_DICT) {
                    | cmp_imm_mem [arg2 + offsetof(PyDictObject, ma_values)], 0
                    | je >1 // fail if dict->ma_values == NULL
                    // _PyDict_GetDictKeyVersionFromSplitDict:
                    // arg3 = arg2->ma_keys
                    | mov arg3, [arg2 + offsetof(PyDictObject, ma_keys)]
                    | cmp_imm_mem [arg3 + offsetof(PyDictKeysObject, dk_version_tag)], la->u.value_cache.dict_ver
                    | jne >1
                } else {
                    | cmp_imm_mem [arg2 + offsetof(PyDictObject, ma_version_tag)], la->u.value_cache.dict_ver
                    | jne >1
                }
                | 2:
                PyObject* r = la->u.value_cache.obj;
                emit_mov_imm(Dst, res_idx, (unsigned long)r);

                // In theory we could remove some of these checks, since we could prove that tp_descr_get wouldn't
                // be able to change.  But we have to do that determination at cache-set time, because that's the
                // only time we know that the cached value is alive.  So it's unclear if it's worth it, especially
                // for the complexity.
                if (la->guard_tp_descr_get) {
                    | mov arg2, [res + offsetof(PyObject, ob_type)]
                    | cmp_imm_mem [arg2 + offsetof(PyTypeObject, tp_descr_get)], 0
                    | jne >1
                }

                emit_incref(Dst, res_idx);
            } else if (la->cache_type == LA_CACHE_IDX_SPLIT_DICT) {
                // arg4 = dict->ma_values
                | mov arg4, [arg2 + offsetof(PyDictObject, ma_values)]
                | test arg4, arg4
                | jz >1 // fail if dict->ma_values == NULL
                // _PyDict_GetDictKeyVersionFromSplitDict:
                // arg3 = arg2->ma_keys
                | mov arg3, [arg2 + offsetof(PyDictObject, ma_keys)]
                | cmp_imm_mem [arg3 + offsetof(PyDictKeysObject, dk_version_tag)], la->u.split_dict_cache.splitdict_keys_version
                | jne >1
                // res = arg4[splitdict_index]
                | mov res, [arg4 + sizeof(PyObject*) * la->u.split_dict_cache.splitdict_index]
                // attr can be NULL
                | test res, res
                | jz >3
                emit_load_attr_res_0_helper = 1; // makes sure we emit label 3
                emit_incref(Dst, res_idx);
            }

            |4:
            if (jit_stats_enabled) {
                | inc qword [opcode == LOAD_ATTR ? &jit_stat_load_attr_hit : &jit_stat_load_method_hit]
            }
            if (opcode == LOAD_ATTR) {
                if (ref_status == OWNED) {
                    emit_decref(Dst, arg1_idx, 1 /*= preserve res */);
                }
            } else {
                if (la->meth_found) {
                    emit_write_vs(Dst, res_idx, 1 /*=top*/);
                    | mov res, arg1
                } else {
                    emit_mov_imm(Dst, tmp_idx, 0);
                    emit_write_vs(Dst, tmp_idx, 1 /*=top*/);
                    emit_decref(Dst, arg1_idx, 1 /*= preserve res */);
                }
            }
            |5:
            deferred_vs_push(Dst, REGISTER, res_idx);
            // fallthrough to next opcode

            switch_section(Dst, SECTION_COLD);
            | 1:
            if (jit_stats_enabled) {
                | inc qword [opcode == LOAD_ATTR ? &jit_stat_load_attr_miss : &jit_stat_load_method_miss]
            }
            if (opcode == LOAD_ATTR) {
                | mov arg2, arg1
                if (ref_status == BORROWED) { // helper function needs a owned value
                    emit_incref(Dst, arg2_idx);
                }
                emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                    arg3_idx, co_opcache);
                emit_aot_func_call(Dst, 3, opcode, oparg, co_opcache != 0 /*= use op cache */);
            } else {
                emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                    arg2_idx, co_opcache);
                emit_aot_func_call(Dst, 2, opcode, oparg, co_opcache != 0 /*= use op cache */);
            }
            emit_if_res_0_error(Dst);
            | jmp <5 // jump to the common code which pushes the result

            if (emit_load_attr_res_0_helper) { // we only emit this code if it's used
                |3:
                | mov preserved_reg, arg1
                emit_mov_imm(Dst, arg2_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
                emit_call_ext_func(Dst, loadAttrCacheAttrNotFound);
                | mov arg1, preserved_reg
                | test res, res
                | jnz <4 // jump to the common code which decrefs the obj and pushes the result
                if (ref_status == OWNED) {
                    emit_decref(Dst, preserved_reg_idx, 0 /*=  don't preserve res */);
                }
                if (jit_stats_enabled) {
                    | inc qword [opcode == LOAD_ATTR ? &jit_stat_load_attr_hit : &jit_stat_load_method_hit]
                }
                | jmp ->error
            }
            switch_section(Dst, SECTION_CODE);

            return 0;
        }
    }
    return 1;
}

// __attribute__((optimize("-O0"))) // enable to make "source tools/dis_jit_gdb.py" work
void* jit_func(PyCodeObject* co, PyThreadState* tstate) {
    if (mem_bytes_used_max <= mem_bytes_used) // stop emitting code we used up all memory
        return NULL;

    // setup jit context, will get accessed from all dynasm functions via the name 'Dst'
    Jit jit;
    memset(&jit, 0, sizeof(jit));
    jit.co = co;
    jit.co_consts = co->co_consts;
    jit.co_names = co->co_names;
    jit.current_section = SECTION_CODE;


    Jit* Dst = &jit;
    dasm_init(Dst, DASM_MAXSECTION);
    |.globals lbl_
    void* labels[lbl__MAX];
    dasm_setupglobal(Dst, labels, lbl__MAX);

    |.actionlist bf_actions
    dasm_setup(Dst, bf_actions);

    // we emit the opcode implementations first and afterwards the entry point of the function because
    // we don't know how much stack it will use etc..
    switch_section(Dst, SECTION_CODE);

    const int num_opcodes = PyBytes_Size(co->co_code)/sizeof(_Py_CODEUNIT);
    // allocate enough space for emitting a dynamic label for the start of every bytecode
    dasm_growpc(Dst,  num_opcodes + 1);
    const _Py_CODEUNIT *first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);

    char* is_jmp_target = calculate_jmp_targets(first_instr, num_opcodes);

    // this keeps track of which fast local variable we know are set (!= 0)
    // if we don't know if they are set or if they are 0 is defined will be 0
    // currently we only track definedness inside a basic block and in addition the function args
    // TODO: could use a bitvector instead of a byte per local variable
    char* known_defined = (char*)malloc(co->co_nlocals);
    int funcs_args_are_always_defined = check_func_args_never_deleted(first_instr, num_opcodes, co->co_argcount);

    int old_line_number = -1;
    int emitted_trace_check_for_line = 0;

    // this is used for the special EXTENDED_ARG opcode
    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx<num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = first_instr[inst_idx];
        int opcode = _Py_OPCODE(word);
        int oparg = _Py_OPARG(word);

        // this is used for the special EXTENDED_ARG opcode
        oparg |= oldoparg;
        oldoparg = 0;

        // if an instruction can jump to this one we need to make sure the deferred stack is clear
        if (is_jmp_target[inst_idx]) {
            deferred_vs_apply(Dst);
        }

        // if we can jump to this opcode or it's the first in the function
        // we reset the definedness info.
        if (ENABLE_DEFINED_TRACKING && (inst_idx == 0 || is_jmp_target[inst_idx])) {
            memset(known_defined, 0, co->co_nlocals);
            for (int i=0; funcs_args_are_always_defined && i<co->co_argcount; ++i) {
                known_defined[i] = 1; // function arg is defined
            }
        }

        // set jump target for current inst index
        // we can later jump here via =>oparg etc..
        // also used for the opcode_addr table
        |=>inst_idx:

        // we don't emit signal and tracing checks for this opcodes
        // because we know they are not calling into any python function.
        // We currently don't do this optimizations for opcodes like STORE_FAST which
        // could call a destructor.

        // The interpreter will only generate a trace line for the first bytecode of line number in the source file.
        // This means that if tracing gets enabled in the middle of a sequence of bytecodes it skips until the start
        // of the next line.
        // Because we don't generate trace checks for some bytecodes we have to manually check
        // if a tracing check is the first for a specific line number even though it may not be the first bytecode
        // for this line.
        // In case it's the first check for a specific line we will overwrite the logic in the interpreter on deopt and
        // force writing out the line trace.
        int current_line_number = PyCode_Addr2Line(co, inst_idx * 2);
        if (current_line_number != old_line_number)
            emitted_trace_check_for_line = 0;
        old_line_number = current_line_number;

        switch (opcode) {
#if ENABLE_AVOID_SIG_TRACE_CHECK
            case NOP:
            case EXTENDED_ARG:
            case ROT_TWO:
            case ROT_THREE:
            case ROT_FOUR:
            case POP_TOP:
            case DUP_TOP:
            case DUP_TOP_TWO:
            case LOAD_CLOSURE:
#if ENABLE_DEFERRED_LOAD_CONST
            case LOAD_CONST:
#endif
                goto skip_sig_trace_check;

#if ENABLE_DEFERRED_LOAD_FAST
            case LOAD_FAST:
                if (known_defined[oparg])
                    goto skip_sig_trace_check; // don't do a sig check if we know the load can't throw
                break;
#endif

#endif // ENABLE_AVOID_SIG_TRACE_CHECK
        }


        // WARNING: if you adjust anything here check if you have to adjust jmp_to_inst_idx

        // set opcode pointer. we do it before checking for signals to make deopt easier
        | mov dword [f + offsetof(PyFrameObject, f_lasti)], inst_idx*2 // inst is 8 bytes long

        // if the current opcode has an EXTENDED_ARG prefix (or more of them - not sure if possible but we handle it here)
        // we need to modify f_lasti in case of deopt.
        // Otherwise the interpreter would skip the EXTENDED_ARG opcodes and would end up generating a completely
        // wrong oparg.
        int num_extended_arg = 0;
#if ENABLE_AVOID_SIG_TRACE_CHECK
        for (int prev_inst_idx = inst_idx-1; prev_inst_idx >= 0 && _Py_OPCODE(first_instr[prev_inst_idx]) == EXTENDED_ARG; --prev_inst_idx)
            ++num_extended_arg;
#endif

        // generate the signal and tracing checks
        switch (opcode) {
        // cpython does not do signal checks for the following opcodes
        // so only generate a trace check for this else test_generators.py will fail
        case SETUP_FINALLY:
        case SETUP_WITH:
        case BEFORE_ASYNC_WITH:
        case YIELD_FROM:
            | cmp dword [interrupt], 0 // inst is 4 bytes long

            // if we deferred stack operations we have to emit a special deopt path
            if (Dst->deferred_vs_next || num_extended_arg) {
                | jne >1
                switch_section(Dst, SECTION_COLD);
                |1:
                deferred_vs_emit(Dst);

                // adjust f_lasti to point to the first EXTENDED_ARG
                if (num_extended_arg) {
                    | mov dword [f + offsetof(PyFrameObject, f_lasti)], (inst_idx-num_extended_arg) *2
                }
            }
            if (!emitted_trace_check_for_line) {
                | jne ->deopt_return_new_line
            } else {
                | jne ->deopt_return
            }
            switch_section(Dst, SECTION_CODE);

            break;

        default:
            {
                // this compares ceval->tracing_possible and eval_breaker in one
                _Static_assert(offsetof(struct _ceval_runtime_state, tracing_possible) == 4, "cmp need to be modified");
                _Static_assert(offsetof(struct _ceval_runtime_state, eval_breaker) == 8, "cmp need to be modified");
                | cmp qword [interrupt], 0 // inst is 4 bytes long

                // if we deferred stack operations we have to emit a special deopt path
                if (Dst->deferred_vs_next || num_extended_arg) {
                    | jne >1
                    switch_section(Dst, SECTION_COLD);
                    |1:
                    | cmp dword [interrupt], 0
                    | je ->handle_signal
                    deferred_vs_emit(Dst);

                    // adjust f_lasti to point to the first EXTENDED_ARG
                    if (num_extended_arg) {
                        | mov dword [f + offsetof(PyFrameObject, f_lasti)], (inst_idx-num_extended_arg) *2
                    }
                    if (!emitted_trace_check_for_line) {
                        | jne ->deopt_return_new_line
                    } else {
                        | jne ->deopt_return
                    }
                    switch_section(Dst, SECTION_CODE);
                } else {
                    if (!emitted_trace_check_for_line) {
                        | jne ->handle_tracing_or_signal_no_deferred_stack_new_line
                    } else {
                        | jne ->handle_tracing_or_signal_no_deferred_stack
                    }
                }
                break;
            }
        }
        emitted_trace_check_for_line = 1;

skip_sig_trace_check:
        switch(opcode) {
        case NOP:
            break;

        case EXTENDED_ARG:
            oldoparg = oparg << 8;
            break;

        case JUMP_ABSOLUTE:
            deferred_vs_apply(Dst);
            emit_jump_to_bytecode_n(Dst, oparg);
            break;

        case JUMP_FORWARD:
            deferred_vs_apply(Dst);
            emit_jump_by_n_bytecodes(Dst, oparg, inst_idx);
            break;

        case LOAD_FAST:
#if ENABLE_DEFERRED_LOAD_FAST
            if (!known_defined[oparg] /* can be null */) {
                | cmp qword [f + get_fastlocal_offset(oparg)], 0
                | je >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_mov_imm(Dst, arg1_idx, oparg); // need to copy it in arg1 because of unboundlocal_error
                | jmp ->unboundlocal_error // arg1 must be oparg!
                switch_section(Dst, SECTION_CODE);

                known_defined[oparg] = 1;
            }

            deferred_vs_push(Dst, FAST, oparg);
#else
            deferred_vs_apply(Dst);
            if (!known_defined[oparg] /* can be null */) {
                | mov arg2, [f + get_fastlocal_offset(oparg)]
                | test arg2, arg2
                | jz >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_mov_imm(Dst, arg1_idx, oparg); // need to copy it in arg1 because of unboundlocal_error
                | jmp ->unboundlocal_error // arg1 must be oparg!
                switch_section(Dst, SECTION_CODE);

                known_defined[oparg] = 1;
            } else {
                | mov arg2, [f + get_fastlocal_offset(oparg)]
            }
            emit_incref(Dst, arg2_idx);
            emit_push_v(Dst, arg2_idx);
#endif
            break;

        case LOAD_CONST:
#if ENABLE_DEFERRED_LOAD_CONST
            deferred_vs_push(Dst, CONST, oparg);
#else
            deferred_vs_apply(Dst);
            PyObject* obj = PyTuple_GET_ITEM(Dst->co_consts, oparg);
            emit_mov_imm(Dst, arg1_idx, (unsigned long)obj);
            if (!is_immortal(obj))
                emit_incref(Dst, arg1_idx);
            emit_push_v(Dst, arg1_idx);
#endif
            break;

        case STORE_FAST:
            deferred_vs_pop1_owned(Dst, arg2_idx);
            deferred_vs_apply_if_same_var(Dst, oparg);
            | lea tmp, [f + get_fastlocal_offset(oparg)]
            | mov arg1, [tmp]
            | mov [tmp], arg2
            if (known_defined[oparg]) {
                emit_decref(Dst, arg1_idx, 0 /* don't preserve res */);
            } else {
                emit_xdecref_arg1(Dst);
            }
            if (ENABLE_DEFINED_TRACKING)
                known_defined[oparg] = 1;

            break;

        case DELETE_FAST:
        {
            deferred_vs_apply_if_same_var(Dst, oparg);
            | lea tmp, [f + get_fastlocal_offset(oparg)]
            | mov arg2, [tmp]
            if (!known_defined[oparg] /* can be null */) {
                | test arg2, arg2
                | jz >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_mov_imm(Dst, arg1_idx, oparg);
                | jmp ->unboundlocal_error // arg1 must be oparg!
                switch_section(Dst, SECTION_CODE);
            }
            | mov qword [tmp], 0
            emit_decref(Dst, arg2_idx, 0 /*= don't preserve res */);

            known_defined[oparg] = 0;

            break;
        }

        case POP_TOP:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            if (ref_status == OWNED) {
                deferred_vs_convert_reg_to_stack(Dst);
                emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
            }
            break;
        }

        case ROT_TWO:
            if (Dst->deferred_vs_next >= 2) {
                DeferredValueStackEntry tmp[2];
                memcpy(tmp, &Dst->deferred_vs[Dst->deferred_vs_next - 2], sizeof(tmp));
                Dst->deferred_vs[Dst->deferred_vs_next - 1] = tmp[0];
                Dst->deferred_vs[Dst->deferred_vs_next - 2] = tmp[1];
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg1_idx, 1 /*=top*/);
                emit_read_vs(Dst, res_idx, 2 /*=second*/);
                emit_write_vs(Dst, arg1_idx, 2 /*=second*/);
                emit_write_vs(Dst, res_idx, 1 /*=top*/);
            }
            break;

        case ROT_THREE:
            if (Dst->deferred_vs_next >= 3) {
                DeferredValueStackEntry tmp[3];
                memcpy(tmp, &Dst->deferred_vs[Dst->deferred_vs_next - 3], sizeof(tmp));
                Dst->deferred_vs[Dst->deferred_vs_next - 1] = tmp[1];
                Dst->deferred_vs[Dst->deferred_vs_next - 2] = tmp[0];
                Dst->deferred_vs[Dst->deferred_vs_next - 3] = tmp[2];
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg1_idx, 1 /*=top*/);
                emit_read_vs(Dst, res_idx, 2 /*=second*/);
                emit_read_vs(Dst, arg3_idx, 3 /*=third*/);
                emit_write_vs(Dst, arg3_idx, 2 /*=second*/);
                emit_write_vs(Dst, arg1_idx, 3 /*=third*/);
                emit_write_vs(Dst, res_idx, 1 /*=top*/);
            }
            break;

        case ROT_FOUR:
            if (Dst->deferred_vs_next >= 4) {
                DeferredValueStackEntry tmp[4];
                memcpy(tmp, &Dst->deferred_vs[Dst->deferred_vs_next - 4], sizeof(tmp));
                Dst->deferred_vs[Dst->deferred_vs_next - 1] = tmp[2];
                Dst->deferred_vs[Dst->deferred_vs_next - 2] = tmp[1];
                Dst->deferred_vs[Dst->deferred_vs_next - 3] = tmp[0];
                Dst->deferred_vs[Dst->deferred_vs_next - 4] = tmp[3];
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg1_idx, 1 /*=top*/);
                emit_read_vs(Dst, res_idx, 2 /*=second*/);
                emit_read_vs(Dst, arg3_idx, 3 /*=third*/);
                emit_read_vs(Dst, arg4_idx, 4 /*=fourth*/);
                emit_write_vs(Dst, arg3_idx, 2 /*=second*/);
                emit_write_vs(Dst, arg4_idx, 3 /*=third*/);
                emit_write_vs(Dst, arg1_idx, 4 /*=fourth*/);
                emit_write_vs(Dst, res_idx, 1 /*=top*/);
            }
            break;

        case DUP_TOP:
            if (Dst->deferred_vs_next >= 1 && Dst->deferred_vs_next + 1 < DEFERRED_VS_MAX &&
                (Dst->deferred_vs[Dst->deferred_vs_next-1].loc == CONST || Dst->deferred_vs[Dst->deferred_vs_next-1].loc == FAST)) {
                Dst->deferred_vs[Dst->deferred_vs_next] = Dst->deferred_vs[Dst->deferred_vs_next-1];
                Dst->deferred_vs_next += 1;
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, res_idx, 1 /*=top*/);
                emit_incref(Dst, res_idx);
                deferred_vs_push(Dst, REGISTER, res_idx);
            }
            break;

        case DUP_TOP_TWO:
            if (Dst->deferred_vs_next >= 2 && Dst->deferred_vs_next + 2 < DEFERRED_VS_MAX &&
                (Dst->deferred_vs[Dst->deferred_vs_next-1].loc == CONST || Dst->deferred_vs[Dst->deferred_vs_next-1].loc == FAST) &&
                (Dst->deferred_vs[Dst->deferred_vs_next-2].loc == CONST || Dst->deferred_vs[Dst->deferred_vs_next-2].loc == FAST)) {
                Dst->deferred_vs[Dst->deferred_vs_next  ] = Dst->deferred_vs[Dst->deferred_vs_next-2];
                Dst->deferred_vs[Dst->deferred_vs_next+1] = Dst->deferred_vs[Dst->deferred_vs_next-1];
                Dst->deferred_vs_next += 2;
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg1_idx, 1 /*=top*/);
                emit_read_vs(Dst, arg2_idx, 2 /*=second*/);
                emit_incref(Dst, arg1_idx);
                emit_incref(Dst, arg2_idx);
                emit_adjust_vs(Dst, 2);
                emit_write_vs(Dst, arg1_idx, 1 /*=top*/);
                emit_write_vs(Dst, arg2_idx, 2 /*=second*/);
            }
            break;

        case RETURN_VALUE:
            deferred_vs_pop1_owned(Dst, res_idx);
            deferred_vs_apply(Dst);
            | jmp ->return
            break;

        case BINARY_MULTIPLY:
        case BINARY_MATRIX_MULTIPLY:
        case BINARY_TRUE_DIVIDE:
        case BINARY_FLOOR_DIVIDE:
        case BINARY_MODULO: // TODO: add special handling like in the interp
        case BINARY_ADD: // TODO: add special handling like in the interp
        case BINARY_SUBTRACT:
        case BINARY_LSHIFT:
        case BINARY_RSHIFT:
        case BINARY_AND:
        case BINARY_XOR:
        case BINARY_OR:
        case BINARY_POWER:

        case INPLACE_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case INPLACE_TRUE_DIVIDE:
        case INPLACE_FLOOR_DIVIDE:
        case INPLACE_MODULO:
        case INPLACE_ADD: // TODO: add special handling like in the interp
        case INPLACE_SUBTRACT:
        case INPLACE_LSHIFT:
        case INPLACE_RSHIFT:
        case INPLACE_AND:
        case INPLACE_XOR:
        case INPLACE_OR:
        case INPLACE_POWER:

        case COMPARE_OP:

        case BINARY_SUBSCR:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg2_idx, arg1_idx, ref_status);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args2(Dst, arg2_idx, arg1_idx, ref_status);
            emit_aot_func_call(Dst, 2, opcode, oparg, 0 /*= no op cache */);
            emit_decref_stored_args(Dst, 2, ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case POP_JUMP_IF_FALSE:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_apply(Dst);

            if (ref_status == OWNED) {
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
                emit_je_to_bytecode_n(Dst, oparg);
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
                | jne >1

                switch_section(Dst, SECTION_COLD);
                |1:
                | mov preserved_reg, arg1
                emit_call_ext_func(Dst, PyObject_IsTrue);
                emit_decref(Dst, preserved_reg_idx, 1 /*= preserve res */);
                | cmp res_32b, 0
                | je =>oparg/2
                | jl ->error
                | jmp >3
                switch_section(Dst, SECTION_CODE);

                |3:
            } else {
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
                emit_je_to_bytecode_n(Dst, oparg);
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
                | jne >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_call_ext_func(Dst, PyObject_IsTrue);
                | cmp res_32b, 0
                | je =>oparg/2
                | jl ->error
                | jmp >3
                switch_section(Dst, SECTION_CODE);

                |3:
            }
            break;
        }

        case POP_JUMP_IF_TRUE:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_apply(Dst);

            if (ref_status == OWNED) {
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
                emit_je_to_bytecode_n(Dst, oparg);
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
                | jne >1

                switch_section(Dst, SECTION_COLD);
                |1:
                | mov preserved_reg, arg1
                emit_call_ext_func(Dst, PyObject_IsTrue);
                emit_decref(Dst, preserved_reg_idx, 1 /*= preserve res */);
                | cmp res_32b, 0
                | jg =>oparg/2
                | jl ->error
                | jmp >3
                switch_section(Dst, SECTION_CODE);


                |3:
            } else {
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
                emit_je_to_bytecode_n(Dst, oparg);
                emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
                | jne >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_call_ext_func(Dst, PyObject_IsTrue);
                | cmp res_32b, 0
                | jg =>oparg/2
                | jl ->error
                | jmp >3
                switch_section(Dst, SECTION_CODE);

                |3:
            }
            break;
        }

        case JUMP_IF_FALSE_OR_POP:
            deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
            emit_je_to_bytecode_n(Dst, oparg);
            emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
            | jne >1

            switch_section(Dst, SECTION_COLD);
            |1:
            emit_call_ext_func(Dst, PyObject_IsTrue);
            | cmp res_32b, 0
            emit_je_to_bytecode_n(Dst, oparg);
            | jl ->error
            | jmp >3
            switch_section(Dst, SECTION_CODE);

            |3:
            emit_pop_v(Dst, arg1_idx);
            emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
            break;

        case JUMP_IF_TRUE_OR_POP:
            deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
            emit_je_to_bytecode_n(Dst, oparg);
            emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
            | jne >1

            switch_section(Dst, SECTION_COLD);
            |1:
            emit_call_ext_func(Dst, PyObject_IsTrue);
            | cmp res_32b, 0
            | jg =>oparg/2
            | jl ->error
            | jmp >3
            switch_section(Dst, SECTION_CODE);

            |3:
            emit_pop_v(Dst, arg1_idx);
            emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
            break;

        case CALL_FUNCTION:
            deferred_vs_apply(Dst);
            | mov arg1, tstate

            // arg2 = &sp
            | mov [rsp], vsp
            | mov arg2, rsp
            emit_mov_imm(Dst, arg3_idx, oparg);
            emit_aot_func_call(Dst, 3, opcode, oparg, 0 /*= no op cache */);

            // stackpointer = sp
            | mov vsp, [rsp]

            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case CALL_METHOD:
            deferred_vs_apply(Dst);
            | mov arg1, tstate

            // arg2 = &sp
            | mov [rsp], vsp
            | mov arg2, rsp

            // this is taken from clang:
            // meth = PEEK(oparg + 2);
            // arg3 = ((meth == 0) ? 0 : 1) + oparg
            emit_mov_imm(Dst, arg3_idx, oparg);
            | cmp qword [vsp - (8*(oparg + 2))], 1
            | sbb arg3, -1

            emit_aot_func_call(Dst, 3, opcode, oparg, 0 /*= no op cache */);
            emit_adjust_vs(Dst, -(oparg + 2));

            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case FOR_ITER:
            deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            | mov tmp, [arg1 + offsetof(PyObject, ob_type)]
            | call qword [tmp + offsetof(PyTypeObject, tp_iternext)]
            | test res, res
            | jz >1

            switch_section(Dst, SECTION_COLD);
            |1:
            emit_call_ext_func(Dst, JIT_HELPER_FOR_ITER_SECOND_PART);
            emit_if_res_0_error(Dst);
            emit_jump_by_n_bytecodes(Dst, oparg, inst_idx);
            switch_section(Dst, SECTION_CODE);

            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_INVERT:
        case GET_ITER:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args1(Dst, arg1_idx, &ref_status);
            emit_aot_func_call(Dst, 1, opcode, oparg, 0 /*= no op cache */);
            emit_decref_stored_args(Dst, 1, &ref_status);

            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case STORE_SUBSCR:
            if (Dst->deferred_vs_next >= 3) {
                RefStatus ref_status[3];
                deferred_vs_pop3(Dst, arg2_idx, arg1_idx, arg3_idx, ref_status);
                deferred_vs_convert_reg_to_stack(Dst);
                emit_store_decref_args3(Dst, arg2_idx, arg1_idx, arg3_idx, ref_status);
                emit_aot_func_call(Dst, 3, opcode, oparg, 0 /*= no op cache */);
                emit_decref_stored_args(Dst, 3, ref_status);
                emit_if_res_32b_not_0_error(Dst);
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg2_idx, 1 /*=top*/);
                emit_read_vs(Dst, arg1_idx, 2 /*=second*/);
                emit_read_vs(Dst, arg3_idx, 3 /*=third*/);
                emit_aot_func_call(Dst, 3, opcode, oparg, 0 /*= no op cache */);
                for (int i=0; i<3; ++i) {
                    emit_read_vs(Dst, arg1_idx, i+1);
                    emit_decref(Dst, arg1_idx, 1 /*= preserve res */);
                }
                emit_adjust_vs(Dst, -3);
                emit_if_res_32b_not_0_error(Dst);
            }
            break;

        case DELETE_SUBSCR:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg2_idx, arg1_idx, ref_status);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args2(Dst, arg2_idx, arg1_idx, ref_status);
            emit_aot_func_call(Dst, 2, opcode, oparg, 0 /*= no op cache */);
            emit_decref_stored_args(Dst, 2, ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case CALL_FINALLY:
            deferred_vs_apply(Dst);
            emit_mov_imm(Dst, arg1_idx, (inst_idx+1) * 2);
            emit_call_ext_func(Dst, PyLong_FromLong);
            emit_if_res_0_error(Dst);
            emit_push_v(Dst, res_idx);
            emit_jump_by_n_bytecodes(Dst, oparg, inst_idx);
            break;

        case END_FINALLY:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_apply(Dst);
            | test arg1, arg1
            | jz >1

            | cmp dword [arg1 + offsetof(PyObject, ob_type)], (unsigned int)&PyLong_Type
            | jne >2

            // inside CALL_FINALLY we created a long with the bytecode offset to the next instruction
            // extract it and jump to it
            emit_store_decref_args1(Dst, arg1_idx, &ref_status);
            emit_call_ext_func(Dst, PyLong_AsLong);
            emit_decref_stored_args(Dst, 1, &ref_status);
            emit_jmp_to_inst_idx(Dst, res_idx, 0 /* don't skip signal and eval_breaker check */);

            |2:
            | mov arg2, arg1
            | mov arg1, tstate
            emit_read_vs(Dst, arg3_idx, 1 /*=top*/);
            emit_read_vs(Dst, arg4_idx, 2 /*=second*/);
            emit_adjust_vs(Dst, -2);
            emit_call_ext_func(Dst, _PyErr_Restore);
            | jmp ->exception_unwind
            |1:
            break;
        }

        case SET_ADD:
        case LIST_APPEND:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg2_idx);
            deferred_vs_peek(Dst, arg1_idx, oparg);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args1(Dst, arg2_idx, &ref_status);
            emit_call_ext_func(Dst, opcode == SET_ADD ? PySet_Add : PyList_Append);
            emit_decref_stored_args(Dst, 1, &ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case MAP_ADD:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg3_idx, arg2_idx, ref_status);
            deferred_vs_peek(Dst, arg1_idx, oparg);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args2(Dst, arg3_idx, arg2_idx, ref_status);
            emit_call_ext_func(Dst, PyDict_SetItem);
            emit_decref_stored_args(Dst, 2, ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case IMPORT_NAME:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg4_idx, arg5_idx, ref_status);
            deferred_vs_convert_reg_to_stack(Dst);
            | mov arg1, tstate
            | mov arg2, f
            emit_mov_imm(Dst, arg3_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
            emit_store_decref_args2(Dst, arg4_idx, arg5_idx, ref_status);
            emit_call_ext_func(Dst, import_name);
            emit_decref_stored_args(Dst, 2, ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case IMPORT_FROM:
            deferred_vs_peek(Dst, arg2_idx, 1);
            deferred_vs_convert_reg_to_stack(Dst);
            | mov arg1, tstate
            emit_mov_imm(Dst, arg3_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
            emit_call_ext_func(Dst, import_from);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case DELETE_ATTR:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_mov_imm(Dst, arg2_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
            emit_mov_imm(Dst, arg3_idx, 0);
            emit_store_decref_args1(Dst, arg1_idx, &ref_status);
            emit_call_ext_func(Dst, PyObject_SetAttr);
            emit_decref_stored_args(Dst, 1, &ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case STORE_GLOBAL:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg3_idx);
            deferred_vs_convert_reg_to_stack(Dst);
            | mov arg1, [f + offsetof(PyFrameObject, f_globals)]
            emit_mov_imm(Dst, arg2_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
            emit_store_decref_args1(Dst, arg3_idx, &ref_status);
            emit_call_ext_func(Dst, PyDict_SetItem);
            emit_decref_stored_args(Dst, 1, &ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case BUILD_SLICE:
        {
            RefStatus ref_status[3];
            if (oparg == 3) {
                deferred_vs_pop3(Dst, arg3_idx, arg2_idx, arg1_idx, ref_status);
            } else {
                emit_mov_imm(Dst, arg3_idx, 0);
                deferred_vs_pop2(Dst, arg2_idx, arg1_idx, &ref_status[1]);
                ref_status[0] = BORROWED;
            }
            deferred_vs_convert_reg_to_stack(Dst);
            emit_store_decref_args3(Dst, arg3_idx, arg2_idx, arg1_idx, ref_status);
            emit_call_ext_func(Dst, PySlice_New);
            emit_decref_stored_args(Dst, 3, ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case BUILD_TUPLE:
            // empty tuple optimization
            if (oparg == 0) {
                PyObject* empty_tuple = PyTuple_New(0);
                deferred_vs_convert_reg_to_stack(Dst);
                emit_mov_imm(Dst, res_idx, (unsigned long)empty_tuple);
                if (!is_immortal(empty_tuple))
                    emit_incref(Dst, res_idx);
                deferred_vs_push(Dst, REGISTER, res_idx);
                Py_DECREF(empty_tuple);
                break;
            }
             __attribute__ ((fallthrough));

        case BUILD_LIST:
            deferred_vs_convert_reg_to_stack(Dst);
            emit_mov_imm(Dst, arg1_idx, oparg);
            emit_call_ext_func(Dst, opcode == BUILD_LIST ? PyList_New : PyTuple_New_Nonzeroed);
            emit_if_res_0_error(Dst);
            if (oparg) {
                // PyTupleObject stores the elements directly inside the object
                // while PyListObject has ob_item which points to an array of elements to support resizing.
                if (opcode == BUILD_LIST) {
                    | mov arg2, [res + offsetof(PyListObject, ob_item)]
                }
                int i = oparg;
                while (--i >= 0) {
                    deferred_vs_peek_owned(Dst, arg1_idx, (oparg - i));
                    if (opcode == BUILD_LIST) {
                        | mov [arg2 + 8*i], arg1
                    } else {
                        | mov [res + offsetof(PyTupleObject, ob_item) + 8*i], arg1
                    }
                }
                deferred_vs_remove(Dst, oparg);
            }
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case LOAD_CLOSURE:
            deferred_vs_convert_reg_to_stack(Dst);
            // PyObject *cell = freevars[oparg];
            emit_load_freevar(Dst, res_idx, oparg);
            emit_incref(Dst, res_idx);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;

        case STORE_DEREF:
            deferred_vs_pop1_owned(Dst, arg2_idx);
            deferred_vs_convert_reg_to_stack(Dst);
            // PyObject *cell = freevars[oparg];
            emit_load_freevar(Dst, arg3_idx, oparg);
            // PyObject *oldobj = PyCell_GET(cell);
            | mov arg1, [arg3 + offsetof(PyCellObject, ob_ref)]
            // PyCell_SET(cell, v);
            | mov [arg3 + offsetof(PyCellObject, ob_ref)], arg2
            emit_xdecref_arg1(Dst);
            break;

        case LOAD_DEREF:
        case DELETE_DEREF:
            deferred_vs_convert_reg_to_stack(Dst);
            // PyObject *cell = freevars[oparg];
            emit_load_freevar(Dst, arg1_idx, oparg);
            // PyObject *value = PyCell_GET(cell);
            | mov res, [arg1 + offsetof(PyCellObject, ob_ref)]
            | test res, res
            | jz >1

            switch_section(Dst, SECTION_COLD);
            |1:
            emit_mov_imm(Dst, arg3_idx, oparg); // load_deref_error assumes that oparg is in arg3!
            | jmp ->deref_error
            switch_section(Dst, SECTION_CODE);

            if (opcode == LOAD_DEREF) {
                emit_incref(Dst, res_idx);
                deferred_vs_push(Dst, REGISTER, res_idx);
            } else { // DELETE_DEREF
                | mov qword [arg1 + offsetof(PyCellObject, ob_ref)], 0
                emit_decref(Dst, res_idx, 0 /*= don't preserve res */);
            }
            break;

        default:
            // compiler complains if the first line after a label is a declaration and not a statement:
            (void)0;

            _PyOpcache* co_opcache = NULL;
            if (co->co_opcache != NULL) {
                unsigned char co_opt_offset = co->co_opcache_map[inst_idx + 1];
                if (co_opt_offset > 0) {
                    assert(co_opt_offset <= co->co_opcache_size);
                    co_opcache = &co->co_opcache[co_opt_offset - 1];
                    assert(co_opcache != NULL);
                }
            }

            // try emitting a IC for the operation if possible
            if (emit_inline_cache(Dst, opcode, oparg, co_opcache) == 0)
                continue;

            // this opcode is implemented via the helpers in aot_ceval_jit_helper.c
            // some take a fixed number of python values as arguments
            switch (opcode) {
                // ### ONE PYTHON ARGS ###
                // JIT_HELPER1
                case UNARY_NOT:
                case PRINT_EXPR:
                case GET_AITER:
                case GET_AWAITABLE:
                case YIELD_FROM:
                case YIELD_VALUE:
                case END_ASYNC_FOR:
                case UNPACK_SEQUENCE:
                case UNPACK_EX:
                case IMPORT_STAR:
                case GET_YIELD_FROM_ITER:
                case SETUP_ASYNC_WITH:
                case CALL_FUNCTION_KW:
                // JIT_HELPER_WITH_NAME1
                case STORE_NAME:
                // JIT_HELPER_WITH_NAME_OPCACHE_AOT1
                case LOAD_ATTR:
                    deferred_vs_pop1_owned(Dst, arg2_idx);
                    break;

                // ### TWO PYTHON ARGS ###
                // JIT_HELPER2
                case WITH_CLEANUP_FINISH:
                case MAKE_FUNCTION:
                // JIT_HELPER_WITH_NAME_OPCACHE_AOT2
                case STORE_ATTR:
                    deferred_vs_pop2_owned(Dst, arg2_idx, arg3_idx);
                    break;

                // ### TWO OR THREE PYTHON ARGS ###
                case CALL_FUNCTION_EX:
                    if (oparg & 1) {
                        deferred_vs_pop3_owned(Dst, arg2_idx, arg3_idx, arg4_idx);
                    } else {
                        deferred_vs_pop2_owned(Dst, arg2_idx, arg3_idx);
                    }
                    break;

                // ### NO PYTHON ARGS ###
                default:
                    break;
            }


            switch (opcode) {
                // the following opcodes don't access the python value stack (no PUSH, POP etc)
                // which means we don't need to create it we just have to spill the 'res' reg if it's used
                case UNARY_NOT:
                case PRINT_EXPR:
                case GET_AITER:
                case GET_AWAITABLE:
                case POP_BLOCK:
                case LOAD_BUILD_CLASS:
                case STORE_NAME:
                case DELETE_NAME:
                case STORE_ATTR:
                case DELETE_GLOBAL:
                case LOAD_NAME:
                case LOAD_GLOBAL:
                case LOAD_CLASSDEREF:
                case SETUP_ANNOTATIONS:
                case LOAD_ATTR:
                case IMPORT_STAR:
                case GET_YIELD_FROM_ITER:
                case CALL_FUNCTION_EX:
                    deferred_vs_convert_reg_to_stack(Dst);
                    break;

                default:
                    deferred_vs_apply(Dst);
            }


            // Some opcodes just use oparg to look up the name and are implemented
            // with JIT_HELPER_WITH_NAME, and for those we pass the statically-resolved
            // name as the first arg and don't pass oparg
            switch (opcode) {
                // *_WITH_NAME:
                case STORE_NAME:
                case DELETE_NAME:
                case STORE_ATTR:
                case DELETE_GLOBAL:
                case LOAD_NAME:
                case LOAD_GLOBAL:
                case LOAD_ATTR:
                case LOAD_METHOD:
                    emit_mov_imm(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
                    break;


                // *_WITH_OPARG:
                case RAISE_VARARGS:
                case POP_FINALLY:
                case UNPACK_SEQUENCE:
                case UNPACK_EX:
                case LOAD_CLASSDEREF:
                case BUILD_STRING:
                case BUILD_TUPLE_UNPACK_WITH_CALL:
                case BUILD_TUPLE_UNPACK:
                case BUILD_LIST_UNPACK:
                case BUILD_SET:
                case BUILD_SET_UNPACK:
                case BUILD_MAP:
                case BUILD_CONST_KEY_MAP:
                case BUILD_MAP_UNPACK:
                case BUILD_MAP_UNPACK_WITH_CALL:
                case SETUP_FINALLY:
                case SETUP_ASYNC_WITH:
                case SETUP_WITH:
                case CALL_FUNCTION_KW:
                case CALL_FUNCTION_EX:
                case MAKE_FUNCTION:
                case FORMAT_VALUE:
                    emit_mov_imm(Dst, arg1_idx, oparg);
                    break;

                default:
                    // For all other opcodes, we don't pass anything in the first arg
                    break;
            }


            switch (opcode) {
                // some of the opcodes called JIT_HELPER_WITH_NAME_OPCACHE_AOT
                // take a 3th arg which is a pointer to _PyOpcache*
                // and a 4th arg which is a pointer to the func address so it can patch itself
                case LOAD_METHOD:
                case LOAD_GLOBAL:
                    // Often the name and opcache pointers are close to each other,
                    // so instead of doing two 64-bit moves, we can do the second
                    // one as a lea off the first one and save a few bytes
                    emit_mov_imm_or_lea(Dst, arg2_idx, arg1_idx, co_opcache, PyTuple_GET_ITEM(Dst->co_names, oparg));
                    emit_aot_func_call(Dst, 2, opcode, oparg, co_opcache != 0 /*= use op cache */);
                    break;

                case LOAD_ATTR:
                    emit_mov_imm_or_lea(Dst, arg3_idx, arg1_idx, co_opcache, PyTuple_GET_ITEM(Dst->co_names, oparg));
                    emit_aot_func_call(Dst, 3, opcode, oparg, co_opcache != 0 /*= use op cache */);
                    break;

                case STORE_ATTR:
                    emit_mov_imm_or_lea(Dst, arg4_idx, arg1_idx, co_opcache, PyTuple_GET_ITEM(Dst->co_names, oparg));
                    emit_aot_func_call(Dst, 4, opcode, oparg, co_opcache != 0 /*= use op cache */);
                    break;


                // default path which nearly all opcodes take: just generate a normal call
                default:
                    emit_call_ext_func(Dst, get_addr_of_helper_func(opcode, oparg));
            }


            // process the return value of the helper function
            switch (opcode) {
                // this opcodes don't continue to the next op but instead branch to special labels
                // they are tightly coupled with the C helper functions
                // be careful when introducing new paths / updating cpython
                case YIELD_FROM:
                case YIELD_VALUE:
                    // res is the PyObject* returned
                    // res == 0 means error
                    // res == 1 means execute next opcode (=fallthrough)
                    // res == 2 means goto exit_yielding
                    emit_if_res_0_error(Dst);
                    | cmp res, 1
                    | jne ->exit_yielding
                    break;

                case RAISE_VARARGS:
                    // res == 0 means error
                    // res == 2 means goto exception_unwind
                    emit_if_res_0_error(Dst);
                    | jmp ->exception_unwind
                    break;

                case END_ASYNC_FOR:
                    // res == 1 means JUMP_BY(oparg) (only other value)
                    // res == 2 means goto exception_unwind
                    | cmp res, 2
                    | je ->exception_unwind
                    emit_jump_by_n_bytecodes(Dst, oparg, inst_idx);
                    break;

                // opcodes helper functions which return the result instead of pushing to the value stack
                case UNARY_NOT:
                case GET_AITER:
                case GET_ANEXT:
                case GET_AWAITABLE:
                case LOAD_BUILD_CLASS:
                case LOAD_NAME:
                case LOAD_GLOBAL:
                case LOAD_CLASSDEREF:
                case BUILD_STRING:
                case BUILD_TUPLE_UNPACK_WITH_CALL:
                case BUILD_TUPLE_UNPACK:
                case BUILD_LIST_UNPACK:
                case BUILD_SET:
                case BUILD_SET_UNPACK:
                case BUILD_MAP:
                case BUILD_CONST_KEY_MAP:
                case BUILD_MAP_UNPACK:
                case BUILD_MAP_UNPACK_WITH_CALL:
                case LOAD_ATTR:
                case GET_YIELD_FROM_ITER:
                case BEFORE_ASYNC_WITH:
                case SETUP_ASYNC_WITH:
                case SETUP_WITH:
                case WITH_CLEANUP_START:
                case LOAD_METHOD:
                case CALL_FUNCTION_KW:
                case CALL_FUNCTION_EX:
                case MAKE_FUNCTION:
                case FORMAT_VALUE:
                    // res == 0 means error
                    // all other values are the returned python object
                    emit_if_res_0_error(Dst);
                    deferred_vs_push(Dst, REGISTER, res_idx);
                    break;

                default:
                    // res == 0 means error
                    // res == 1 means execute next opcode (=fallthrough)
                    emit_if_res_0_error(Dst);
                    break;
            }
        }
    }

    ////////////////////////////////
    // CALCULATE NUMBER OF STACK VARIABLES
    // stack must be aligned which means it must be a uneven number of slots!
    // (because a call will push the return address to the stack which makes it aligned)
    unsigned long num_stack_slots = Dst->num_deferred_stack_slots + NUM_MANUAL_STACK_SLOTS;
    if ((num_stack_slots & 1) == 0)
        ++num_stack_slots;


    ////////////////////////////////
    // EPILOG OF EMITTED CODE: jump target to different exit path
    |->epilog:
    // TODO: only emit a label if we actually generated an instruction which needs it
    |->exception_unwind:
    emit_mov_imm(Dst, res_idx, 1);
    | jmp ->return

    |->exit_yielding:
    // to differentiate from a normal return we set the second lowest bit
    | or res, 2
    | jmp ->return

    |->handle_signal:
    // we have to preserve res because it may be used by our deferred stack optimizations
    | mov preserved_reg, res
    emit_call_ext_func(Dst, eval_breaker_jit_helper);
    emit_if_res_32b_not_0_error(Dst);
    | mov res, preserved_reg
    emit_jmp_to_lasti(Dst, 1 /* skip signal and eval_breaker check*/);

    |->handle_tracing_or_signal_no_deferred_stack:
    | cmp dword [interrupt], 0
    | je ->handle_signal
    | jmp ->deopt_return

    |->handle_tracing_or_signal_no_deferred_stack_new_line:
    | cmp dword [interrupt], 0
    | je ->handle_signal
    // falltrough

    |->deopt_return_new_line:
    emit_mov_imm(Dst, res_idx, 1 << 3 /* this means first trace check for this line */ | 3 /*= deopt */);
    | jmp ->return

    |->deopt_return:
    emit_mov_imm(Dst, res_idx, 3 /*= deopt */);
    | jmp ->return

    |->deref_error: // assumes that oparg is in arg3!
    | mov arg1, tstate
    emit_mov_imm(Dst, arg2_idx, (unsigned long)co);
    emit_call_ext_func(Dst, format_exc_unbound);
    | jmp ->error

    // we come here if the result of LOAD_FAST or DELETE_FAST is null
    |->unboundlocal_error:
    // arg1 must be oparg!
    emit_call_ext_func(Dst, JIT_HELPER_UNBOUNDLOCAL_ERROR);
    // fallthrough to error

    |->error:
    // we have to decref all python object stored in the deferred stack array
    | mov arg1, preserved_reg2
    emit_xdecref_arg1(Dst);
    for (int i=0; i<Dst->num_deferred_stack_slots; ++i) {
        | mov arg1, [rsp + (NUM_MANUAL_STACK_SLOTS + i) * 8]
        emit_xdecref_arg1(Dst);
    }

    emit_mov_imm(Dst, res_idx, 0);

    |->return:
    // ret value one must already be set
    // second is the value stackpointer
    | mov res2, vsp


    // remove stack variable
    | add rsp, num_stack_slots*8

    // restore callee saves
    | pop interrupt
    | pop vsp
    | pop tstate
    | pop f
    | pop preserved_reg
    | pop preserved_reg2
    | ret


    ////////////////////////////////
    // ENTRY OF EMITTED FUNCTION
    switch_section(Dst, SECTION_ENTRY);
    |.align 16
    |->entry:
    // callee saves
    | push preserved_reg2
    | push preserved_reg
    | push f
    | push tstate
    | push vsp
    | push interrupt

    // Signature:
    // (PyFrameObject* f, PyThread* tstate, PyObject** sp){
    | mov f, arg1
    | mov tstate, arg2
    | mov vsp, arg3

    // allocate stack variables
    | sub rsp, num_stack_slots*8

    // We store the address of _PyRuntime.ceval.tracing_possible and eval_break inside a register
    // this makes it possible to compare this two 4 byte variables at the same time to 0
    // via one 4 byte long (machine code size) 'cmp qword [interrupt], 0' instruction
    // (using the address as immediate instead of the reg would require 8/9bytes)
    // and this adds up because we emit it infront of every opcode.
    // The offsets of the fields are not important as long as the two fields are next to each other and fit in 8bytes -> assert is overly strict
    _Static_assert(offsetof(struct _ceval_runtime_state, tracing_possible) == 4, "");
    _Static_assert(offsetof(struct _ceval_runtime_state, eval_breaker) == 8, "");
    _Static_assert(sizeof(((struct _ceval_runtime_state*)0)->tracing_possible) == 4, "");
    _Static_assert(sizeof(((struct _ceval_runtime_state*)0)->eval_breaker) == 4, "");

    emit_mov_imm(Dst, interrupt_idx, &_PyRuntime.ceval.tracing_possible);

    // clear deferred stack space (skip manual stack slots because they don't need to be zero)
    // we clear it so in case of error we can just decref this space
    emit_mov_imm(Dst, preserved_reg2_idx, 0);
    for (int i=0; i<Dst->num_deferred_stack_slots; ++i) {
        | mov qword [rsp + (NUM_MANUAL_STACK_SLOTS + i) * 8], 0
    }

    // jumps either to first opcode implementation or resumes a generator
    emit_jmp_to_lasti(Dst, 0 /* don't skip signal and eval_breaker check */);


    ////////////////////////////////
    // OPCODE TABLE

    // table of bytecode index -> IP
    // used e.g. for continuing generators
    switch_section(Dst, SECTION_OPCODE_ADDR);
    |->opcode_addr_begin:
    for (int inst_idx=0; inst_idx<num_opcodes; ++inst_idx) {
        // emit 4byte address to start of implementation of instruction with index 'inst_idx'
        // - this is fine our addresses are only 4 byte long not 8
        |.aword =>inst_idx
    }

    size_t size;
    dasm_link(Dst, &size);

    // Align code regions to cache line boundaries.
    // I don't know concretely that this is important but seems like
    // something maybe you're supposed to do?
    size = (size + 15) / 16 * 16;

    // Allocate jitted code regions in 256KB chunks:
    if (size > mem_chunk_bytes_remaining) {
        mem_chunk_bytes_remaining = size > (1<<18) ? size : (1<<18);

        // allocate memory which address fits inside a 32bit pointer (makes sure we can use 32bit rip relative adressing)
        mem_chunk = mmap(0, mem_chunk_bytes_remaining, PROT_READ | PROT_WRITE, MAP_32BIT | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        // we self modify (=AOTFuncs) so make it writable to
        mprotect(mem_chunk, mem_chunk_bytes_remaining, PROT_READ | PROT_EXEC | PROT_WRITE);

        mem_bytes_allocated += (mem_chunk_bytes_remaining + 4095) / 4096 * 4096;
    }

    void* mem = mem_chunk;
    mem_chunk += size;
    mem_chunk_bytes_remaining -= size;
    mem_bytes_used += size;

    dasm_encode(Dst, mem);

    if (perf_map_file) {
        PyObject *type, *value, *traceback;
        PyErr_Fetch(&type, &value, &traceback);

        _Py_static_string(PyId_slash, "/");
        PyObject *slash = _PyUnicode_FromId(&PyId_slash); /* borrowed */
        PyObject *partitioned = PyUnicode_RPartition(co->co_filename, slash);

        // Function naming: there are a couple ways we can name the functions.
        // Ideally we would have access to the function object, and would do
        // function.__module__ + "." + function.__qualname__
        // The code object just has the function name + file name, so use that
        // plus the line number for now.  Maybe it's too much info.
        // Add an index at the end in the case of a name clash.
        PyObject* function_name = PyUnicode_FromFormat("%U:%d:%U",
                PyTuple_GET_ITEM(partitioned, 2), co->co_firstlineno, co->co_name);
        PyObject* function_name_bytes = PyUnicode_AsASCIIString(function_name);
        char* function_name_cstr = PyBytes_AS_STRING(function_name_bytes);

        ++perf_map_num_funcs;
        perf_map_funcs = realloc(perf_map_funcs, perf_map_num_funcs*sizeof(struct PerfMapEntry));
        perf_map_funcs[perf_map_num_funcs-1].func_name = strdup(function_name_cstr);
        perf_map_funcs[perf_map_num_funcs-1].func_size = size;
        perf_map_funcs[perf_map_num_funcs-1].func_addr = mem;

        Py_DECREF(function_name);
        Py_DECREF(function_name_bytes);

        Py_DECREF(partitioned);
        PyErr_Restore(type, value, traceback);
    }

    if (perf_map_opcode_map) {
        // table of bytecode index -> IP (4byte address)
        unsigned int* opcode_addr_begin = (unsigned int*)labels[lbl_opcode_addr_begin];

        // write addr and opcode info into a file which tools/perf_jit.py uses
        // to annotate the 'perf report' output
        for (int inst_idx = 0; inst_idx<num_opcodes; ++inst_idx) {
            _Py_CODEUNIT word = first_instr[inst_idx];
            int opcode = _Py_OPCODE(word);
            int oparg = _Py_OPARG(word);
            void* addr = (void*)(unsigned long)opcode_addr_begin[inst_idx];
            const char* jmp_dst = is_jmp_target[inst_idx] ? "->" : "  ";
            fprintf(perf_map_opcode_map, "%p,%s %4d %-30s %d\n",
                    addr, jmp_dst, inst_idx*2, get_opcode_name(opcode), oparg);
        }
    }

    dasm_free(Dst);
    free(is_jmp_target);
    is_jmp_target = NULL;
    free(known_defined);
    known_defined = NULL;

    return labels[lbl_entry];
}

void show_jit_stats() {
    fprintf(stderr, "jit: %ld bytes used (%.1f%% of allocated)\n", mem_bytes_used, 100.0 * mem_bytes_used / mem_bytes_allocated);
    fprintf(stderr, "jit: inlined %lu (of total %lu) LOAD_ATTR caches: %lu hits %lu misses\n", jit_stat_load_attr_inline, jit_stat_load_attr_total, jit_stat_load_attr_hit, jit_stat_load_attr_miss);
    fprintf(stderr, "jit: inlined %lu (of total %lu) LOAD_METHOD caches: %lu hits %lu misses\n", jit_stat_load_method_inline, jit_stat_load_method_total, jit_stat_load_method_hit, jit_stat_load_method_miss);
    fprintf(stderr, "jit: inlined %lu (of total %lu) LOAD_GLOBAL caches: %lu hits %lu misses\n", jit_stat_load_global_inline, jit_stat_load_global_total, jit_stat_load_global_hit, jit_stat_load_global_miss);
}

void jit_start() {
    if (getenv("JIT_PERF_MAP") != NULL) {
        char buf[80];
        snprintf(buf, 80, "/tmp/perf-%d.map", getpid());
        perf_map_file = fopen(buf, "w");

        system("rm -rf /tmp/perf_map");
        system("mkdir /tmp/perf_map");

        FILE* executable_file = fopen("/tmp/perf_map/executable.txt", "w");
        PyObject* executable = PySys_GetObject("executable");
        PyObject_Print(executable, executable_file, Py_PRINT_RAW);
        fclose(executable_file);

        perf_map_opcode_map = fopen("/tmp/perf_map/opcode_map.txt", "w");
    }
    char* val = getenv("JIT_MAX_MEM");
    if (val) {
        mem_bytes_used_max = atol(val);
    }
    val = getenv("SHOW_JIT_STATS");
    jit_stats_enabled = val && atoi(val);

    val = getenv("JIT_USE_AOT");
    if (val) {
        jit_use_aot = atoi(val);
    }
}

void jit_finish() {
    if (jit_stats_enabled)
        show_jit_stats();

    if (perf_map_file) {
        // dump emitted functions for 'perf report'
        for (int i=0; i<perf_map_num_funcs; ++i) {
            struct PerfMapEntry* entry = &perf_map_funcs[i];
            char fn[120];
            char func_name_unique[120];
            int index = 0;
            do {
                snprintf(func_name_unique, sizeof(func_name_unique), index == 0 ? "%s" : "%s.%d", entry->func_name, index);

                snprintf(fn, sizeof(fn), "/tmp/perf_map/%s", func_name_unique);
                ++index;
             } while (access(fn, F_OK) != -1);

            fprintf(perf_map_file, "%lx %lx %s\n", (uintptr_t)entry->func_addr, entry->func_size, func_name_unique);

            FILE* data_f = fopen(fn, "wb");
            if (data_f) {
                fwrite(entry->func_addr, 1, entry->func_size, data_f);
                fclose(data_f);
            }
            free(entry->func_name);
        }
        free(perf_map_funcs);
        fclose(perf_map_file);
    }

    if (perf_map_opcode_map)
        fclose(perf_map_opcode_map);
}
