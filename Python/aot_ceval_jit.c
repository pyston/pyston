// This file contains the JIT compiler
// The main design choices/goals are:
//  - removing the dispatching overhead of the interpreter
//  - reducing reference counting overhead
//  - reducing overhead related to Python value stack push/pops
//  - inlined caches
//  - generates calls to ahead of time generated type specific traces of bytescodes
//    - e.g. 'float * float' -> PyNumber_MultiplyFloatFloat2
//  - very fast compilation speed
//    - uses DynAsm - good documentation at: https://corsix.github.io/dynasm-doc/index.html
//    - jit thresholds are very low
//    - most bytecode instructions are complex and are handled by calls to functions
//      which means even a more clever compiler can't do much better (when not inlining)
//    - mostly using static register allocation is fine
//      because of all the external calls which restrict register assignment
//    - machine code gets emitted immediately, no IR, passes etc..
//  - can switch from interpreter to compiled function at
//    - function entry
//    - any bytecode which is the target of jump from another bytecode
//  - deoptimization is possible at the start of every bytecode
//    - currently used to implement cpythons 'tracing' support
//    - main task is creating the python value stack for the interpreter to continue
//
// Some assumptions:
//   - we currently only support amd64 systems with SystemV calling convention
//   - code gets emitted into memory area which fits into 32bit address
//     - makes sure we can use relative addressing most of the time
//     - saves some space
//   - we use a custom calling convention to make external calls fast:
//     - args get past/returned following the SystemV calling convention
//     - in addition we have the following often used values in fixed callee saved registers
//       (which means we can call normal C function without changes because have to save them):
//       - r12 - PyObject** python value stack pointer
//       - r13 - PyFrameObject* frame object of currently executing function
//       - r15 - PyThreadState* tstate of currently executing thread
//     - code inside aot_ceval_jit_helper.c is using this special convention
//       via gcc global register variable extension
//       (which prevents us from using LTO on that translation unit)
//
// 'Deferred value stack':
//   - we don't emit python value stack operations (PUSH(), POP(),...) immediately
//     like the interpreter does instead we defer them until the use
//   - this removes many memory operations
//   - we can directly use the correct register when doing a external call
//   - we can avoid some reference counting updates
//   - code which needs the interpreters value stack needs to call:
//     - deferred_vs_apply()
//   - if a instruction modifies register 'res' (=rax) it needs to first call:
//     - deferred_vs_convert_reg_to_stack()
//
// Future bigger ideas:
//    - we support deoptimizations at the start of every bytecode which we could use
//      to generate faster more specialiced code which makes some assumptions
//    - get ride of frame->f_lasti updates
//    - update/overwrite inline cache entries when they start to fail
//

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

// enable runtime checks to catch jit compiler bugs
//#define JIT_DEBUG 1

#if JIT_DEBUG
#define DASM_CHECKS 1
#define JIT_ASSERT(x, m) do { if (!(x)) {                       \
                                fprintf(stderr, "%s\n", m);     \
                                assert(0);                      \
                                abort();                        \
                              }                                 \
                         } while(0)

#else
#define JIT_ASSERT(x, m) assert(x)
#endif

#define DEFERRED_VS_MAX         16 /* used by STORE_SUBSCR */
#define NUM_MANUAL_STACK_SLOTS   2 /* used by STORE_SUBSCR */

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

    // =1 if an entry in deferred_vs is using the vs_preserved_reg
    int deferred_vs_preserved_reg_used;

    // =1 if an entry in deferred_vs is using the 'res' register
    int deferred_vs_res_used;

    int num_opcodes;
    const _Py_CODEUNIT *first_instr;

    char* is_jmp_target; // need to be free()d

    // this keeps track of which fast local variable we know are set (!= 0)
    // if we don't know if they are set or if they are 0 is defined will be 0
    // currently we only track definedness inside a basic block and in addition the function args
    // TODO: could use a bitvector instead of a byte per local variable
    char* known_defined; // need to be free()d

    // used by emit_instr_start to keep state across calls
    int old_line_number;
    int emitted_trace_check_for_line;
} Jit;

#define Dst_DECL Jit* Dst
#define Dst_REF Dst->d

#include <dynasm/dasm_proto.h>
#include <dynasm/dasm_x86.h>

#include <sys/mman.h>
#include <ctype.h>

#include "aot.h"
#include "aot_ceval_jit_helper.h"

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

static int is_immortal(PyObject* obj) {
    return obj->ob_refcnt > (1L<<59);
}

static void* __attribute__ ((const)) get_addr_of_helper_func(int opcode, int oparg) {
    switch (opcode) {
#define JIT_HELPER_ADDR(name)   case name: return JIT_HELPER_##name
        JIT_HELPER_ADDR(UNARY_NOT);
        JIT_HELPER_ADDR(PRINT_EXPR);
        JIT_HELPER_ADDR(RAISE_VARARGS);
        JIT_HELPER_ADDR(GET_AITER);
        JIT_HELPER_ADDR(GET_ANEXT);
        JIT_HELPER_ADDR(GET_AWAITABLE);
        JIT_HELPER_ADDR(YIELD_FROM);
        JIT_HELPER_ADDR(YIELD_VALUE);
        JIT_HELPER_ADDR(POP_EXCEPT);
        JIT_HELPER_ADDR(POP_FINALLY);
        JIT_HELPER_ADDR(END_ASYNC_FOR);
        JIT_HELPER_ADDR(LOAD_BUILD_CLASS);
        JIT_HELPER_ADDR(STORE_NAME);
        JIT_HELPER_ADDR(DELETE_NAME);
        JIT_HELPER_ADDR(UNPACK_EX);
        JIT_HELPER_ADDR(DELETE_GLOBAL);
        JIT_HELPER_ADDR(LOAD_NAME);
        JIT_HELPER_ADDR(LOAD_CLASSDEREF);
        JIT_HELPER_ADDR(BUILD_STRING);
        JIT_HELPER_ADDR(BUILD_TUPLE_UNPACK_WITH_CALL);
        JIT_HELPER_ADDR(BUILD_TUPLE_UNPACK);
        JIT_HELPER_ADDR(BUILD_LIST_UNPACK);
        JIT_HELPER_ADDR(BUILD_SET);
        JIT_HELPER_ADDR(BUILD_SET_UNPACK);
        JIT_HELPER_ADDR(BUILD_MAP);
        JIT_HELPER_ADDR(SETUP_ANNOTATIONS);
        JIT_HELPER_ADDR(BUILD_CONST_KEY_MAP);
        JIT_HELPER_ADDR(BUILD_MAP_UNPACK);
        JIT_HELPER_ADDR(BUILD_MAP_UNPACK_WITH_CALL);
        JIT_HELPER_ADDR(IMPORT_STAR);
        JIT_HELPER_ADDR(GET_YIELD_FROM_ITER);
        JIT_HELPER_ADDR(BEFORE_ASYNC_WITH);
        JIT_HELPER_ADDR(SETUP_WITH);
        JIT_HELPER_ADDR(WITH_CLEANUP_START);
        JIT_HELPER_ADDR(WITH_CLEANUP_FINISH);
        JIT_HELPER_ADDR(CALL_FUNCTION_KW);
        JIT_HELPER_ADDR(MAKE_FUNCTION);
        JIT_HELPER_ADDR(FORMAT_VALUE);

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
#undef JIT_HELPER_ADDR
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

// looks which instruction can be reached by jumps
// this is important for the deferred stack operations because
// if a instruction can be reached by a jump we can't use this optimization.
// which means we have make sure to call deferred_vs_apply.
//
// result must be freed()
static char* calculate_jmp_targets(Jit* Dst) {
    // TODO: could be a bit vector
    char* is_jmp_target = malloc(Dst->num_opcodes);
    memset(is_jmp_target, 0, Dst->num_opcodes);

    // first instruction is always a jump target because entry can reach it
    if (Dst->num_opcodes > 0)
        is_jmp_target[0] = 1;

    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx < Dst->num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = Dst->first_instr[inst_idx];
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
            case END_ASYNC_FOR:
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
static int check_func_args_never_deleted(Jit* Dst) {
    const int num_args = Dst->co->co_argcount;
    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx < Dst->num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = Dst->first_instr[inst_idx];
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
static int jit_num_funcs = 0;

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
// section layout is same as specified here from left to right
|.section entry, code, cold, opcode_addr

////////////////////////////////
// REGISTER DEFINITIONS

// all this values are in callee saved registers
// NOTE: r13 and rbp need 1 byte more to encode a direct memory access without offset
// e.g. mov rax, [rbp] is encoded as mov rax, [rbp + 0]
|.define f, r13 // PyFrameObject*

// this register gets used when we have to make a call but preserve a value across it.
// It can never get used in the deferred_vs / will never get used across bytecode instructions
// One has to manually check the surrounding code if it's safe to use this register.
// This register will not automatically get xdecrefed on error / return
// so no need to clear it after use.
|.define tmp_preserved_reg, rbp
#define tmp_preserved_reg_idx 5

// this register gets mainly used by deferred_vs when we have to make a call
// but preserve a value which is inside the 'res' register (same as stack slot entry but faster).
// Code needs to always check Dst->deferred_vs_preserved_reg_used to see if it's available.
// On error or return we will always xdecref this register which means that
// code must manually clear the register if it does not want the decref.
|.define vs_preserved_reg, r14
#define vs_preserved_reg_idx 14

|.define tstate, r15 // PyThreadState*
|.define vsp, r12 // PyObject** - python value stack pointer

// pointer to ceval->tracing_possible
|.define interrupt, rbx // if you change this you may have to adjust jmp_to_inst_idx
#define interrupt_idx 3


// follow AMD64 calling convention
// instruction indices can be found here: https://corsix.github.io/dynasm-doc/instructions.html
// function arguments
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
|.define arg6, r9  // careful same as register 'tmp'
#define arg6_idx 9

// return values
|.define res, rax
#define res_idx 0
|.define res2, rdx // second return value
|.define res_32b, eax

// will be used by macros
|.define tmp, arg6
#define tmp_idx arg6_idx

// GET_DEFERRED[-1] == top deferred stack entry
// GET_DEFERRED[-2] == second deferred stack entry..
#define GET_DEFERRED (&Dst->deferred_vs[Dst->deferred_vs_next])

static void deferred_vs_apply(Jit* Dst);

static void switch_section(Jit* Dst, Section new_section) {
    JIT_ASSERT(Dst->current_section != new_section, "safe todo but may be sign of a bug");

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
        JIT_ASSERT(0, "unknwon section");
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

static void emit_dec_qword_ptr(Jit* Dst, void* ptr, int can_use_tmp_reg) {
    // the JIT always emits code to address which fit into 32bit
    // but if PIC is enabled non JIT code may use a larger address space.
    // This causes issues because x86_64 rip memory access only use 32bit offsets.
    // To solve this issue we have to load the pointer into a register.
    if (IS_32BIT_VAL(ptr)) {
        | dec qword [&ptr]
    } else {
        // unfortunately we can't modify any register here :/
        // which means we will have to safe an restore via the stack
        if (!can_use_tmp_reg) {
            | push tmp
        }
        | mov64 tmp, (unsigned long)ptr
        | dec qword [tmp]
        if (!can_use_tmp_reg) {
            | pop tmp
        }
    }
}
static void emit_inc_qword_ptr(Jit* Dst, void* ptr, int can_use_tmp_reg) {
    if (IS_32BIT_VAL(ptr)) {
        | inc qword [&ptr]
    } else {
        if (!can_use_tmp_reg) {
            | push tmp
        }
        | mov64 tmp, (unsigned long)ptr
        | inc qword [tmp]
        if (!can_use_tmp_reg) {
            | pop tmp
        }
    }
}

static void emit_incref(Jit* Dst, int r_idx) {
    _Static_assert(offsetof(PyObject, ob_refcnt) == 0,  "add needs to be modified");
#ifdef Py_REF_DEBUG
    // calling code assumes that we are not modifying tmp_reg
    _Static_assert(sizeof(_Py_RefTotal) == 8,  "adjust inc qword");
    emit_inc_qword_ptr(Dst, &_Py_RefTotal, 0 /*=can't use tmp_reg*/);
#endif
    | inc qword [Rq(r_idx)]
}

static void emit_if_res_0_error(Jit* Dst) {
    JIT_ASSERT(Dst->deferred_vs_res_used == 0, "error this would not get decrefed");
    | test res, res
    | jz ->error
}

static void emit_if_res_32b_not_0_error(Jit* Dst) {
    JIT_ASSERT(Dst->deferred_vs_res_used == 0, "error this would not get decrefed");
    | test res_32b, res_32b
    | jnz ->error
}

static void emit_jump_by_n_bytecodes(Jit* Dst, int num_bytes, int inst_idx) {
    int dst_idx = num_bytes/2+inst_idx+1;
    JIT_ASSERT(Dst->is_jmp_target[dst_idx], "calculate_jmp_targets needs adjustment");
    JIT_ASSERT(dst_idx >= 0 && dst_idx < Dst->num_opcodes, "");
    | jmp =>dst_idx
}

static void emit_jump_to_bytecode_n(Jit* Dst, int num_bytes) {
    int dst_idx = num_bytes/2;
    JIT_ASSERT(Dst->is_jmp_target[dst_idx], "calculate_jmp_targets needs adjustment");
    JIT_ASSERT(dst_idx >= 0 && dst_idx < Dst->num_opcodes, "");
    | jmp =>dst_idx
}

static void emit_je_to_bytecode_n(Jit* Dst, int num_bytes) {
    int dst_idx = num_bytes/2;
    JIT_ASSERT(Dst->is_jmp_target[dst_idx], "calculate_jmp_targets needs adjustment");
    JIT_ASSERT(dst_idx >= 0 && dst_idx < Dst->num_opcodes, "");
    | je =>dst_idx
}

static void emit_jg_to_bytecode_n(Jit* Dst, int num_bytes) {
    int dst_idx = num_bytes/2;
    JIT_ASSERT(Dst->is_jmp_target[dst_idx], "calculate_jmp_targets needs adjustment");
    JIT_ASSERT(dst_idx >= 0 && dst_idx < Dst->num_opcodes, "");
    | jg =>dst_idx
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
// of another register `other_idx` which contains a known value `other_addr`
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
    // WARNING: if you modify this you have to adopt SET_JIT_AOT_FUNC
    if (IS_32BIT_SIGNED_VAL((long)addr)) {
        // This emits a relative call. The dynasm syntax is confusing
        // it will not actually take the address of addr (even though it says &addr).
        // Put instead just take the value of addr and calculate the difference to the emitted instruction address. (generated code: dasm_put(Dst, 135, (ptrdiff_t)(addr)))

        // We emit a relative call to the destination function here which can be patched.
        // The address of the call is retrieved via __builtin_return_address(0) inside the AOT func and
        // then the destination of the call instruction (=relative address of the function to call) is modified.
        | call qword &addr // 5byte inst
    } else {
        | mov64 res, (unsigned long)addr
        | call res // compiles to: 0xff 0xd0
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
    emit_dec_qword_ptr(Dst, &_Py_RefTotal, 1 /* can_use_tmp_reg  */);
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
        | mov tmp_preserved_reg, res // save the result
    }

    // inline _Py_Dealloc
    //  call_ext_func _Py_Dealloc
    | mov res, [arg1 + offsetof(PyObject, ob_type)]
    | call qword [res + offsetof(PyTypeObject, tp_dealloc)]

    if (preserve_res) {
        | mov res, tmp_preserved_reg
    }

    if (use_inline_decref) {
        |8:
    } else {
        | jmp >8
        switch_section(Dst, SECTION_CODE);
        |8:
    }
}

static void emit_xdecref_arg1(Jit* Dst) {
    | test arg1, arg1
    | jz >9
    emit_decref(Dst, arg1_idx, 0 /* don't preserve res */);
    |9:
}

// emits a call afterwards decrefs OWNED arguments
// regs and ref_status arrays must be at least num entries long
static void emit_call_decref_args(Jit* Dst, void* func, int num, int regs[], RefStatus ref_status[]) {
    // we have to move owned args to preserved regs (callee saved regs) or to slack slots
    // to be able to decref after the call

    // if no deferred vs entry is using vs_preserved_reg we can use it because we don't generate new
    // deferred vs entries inside this function and will release it afterwards so nobody
    // will notice we used it here temporarily.
    // Priorities for storage location are:
    //   - tmp_preserved_reg
    //   - vs_preserved_reg - if available
    //   - stack slot
    const int can_use_vs_preserved_reg = Dst->deferred_vs_preserved_reg_used == 0;

    for (int i=0, num_owned = 0; i<num; ++i) {
        if (ref_status[i] != OWNED)
            continue;
        if (num_owned == 0) {
            | mov tmp_preserved_reg, Rq(regs[i])
        } else if (num_owned == 1 && can_use_vs_preserved_reg) {
            | mov vs_preserved_reg, Rq(regs[i])
        } else {
            int stack_slot = num_owned - can_use_vs_preserved_reg -1;
            | mov [rsp + stack_slot*8], Rq(regs[i])
        }
        ++num_owned;
    }

    emit_call_ext_func(Dst, func);

    for (int i=0, num_decref=0; i<num; ++i) {
        if (ref_status[i] != OWNED)
            continue;
        if (num_decref == 0) {
            emit_decref(Dst, tmp_preserved_reg_idx, 1); /*= preserve res */
        } else if (num_decref == 1 && can_use_vs_preserved_reg) {
            emit_decref(Dst, vs_preserved_reg_idx, 1); /*= preserve res */
            emit_mov_imm(Dst, vs_preserved_reg_idx, 0); // we have to clear it because error path will xdecref
        } else {
            int stack_slot = num_decref - can_use_vs_preserved_reg -1;
            // this should never happen if it does adjust NUM_MANUAL_STACK_SLOTS
            JIT_ASSERT(stack_slot < NUM_MANUAL_STACK_SLOTS, "");
            | mov arg1, [rsp + stack_slot*8]
            emit_decref(Dst, arg1_idx, 1); /*= preserve res */
        }
        ++num_decref;
    }
}
static void emit_call_decref_args1(Jit* Dst, void* func, int r1_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx };
    emit_call_decref_args(Dst, func, 1, regs, ref_status);
}
static void emit_call_decref_args2(Jit* Dst, void* func, int r1_idx, int r2_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx, r2_idx };
    emit_call_decref_args(Dst, func, 2, regs, ref_status);
}
static void emit_call_decref_args3(Jit* Dst, void* func, int r1_idx, int r2_idx, int r3_idx, RefStatus ref_status[]) {
    int regs[] = { r1_idx, r2_idx, r3_idx };
    emit_call_decref_args(Dst, func, 3, regs, ref_status);
}

static void* get_aot_func_addr(Jit* Dst, int opcode, int oparg, int opcache_available) {
    return get_addr_of_aot_func(opcode, oparg, opcache_available);
}

static void emit_mov_inst_addr(Jit* Dst, int r_dst, int r_inst_idx) {
    // *2 instead of *4 because:
    // entries are 4byte wide addresses but lasti needs to be divided by 2
    // because it tracks offset in bytecode (2bytes long) array not the index
    | mov Rd(r_dst), [Rq(r_inst_idx)*2 + ->opcode_addr_begin]
}

static void emit_jmp_to_inst_idx(Jit* Dst, int r_idx) {
    JIT_ASSERT(r_idx != tmp_idx, "can't be tmp");

    emit_mov_inst_addr(Dst, tmp_idx, r_idx);
    | jmp tmp
}

#if JIT_DEBUG
static void debug_error_not_a_jump_target(PyFrameObject* f) {
    fprintf(stderr, "ERROR: jit entry points to f->f_lasti %d which is not a jump target\n", f->f_lasti);
    JIT_ASSERT(0, "");
}
#endif

// warning this will overwrite tmp and arg1
static void emit_jmp_to_lasti(Jit* Dst) {
    | mov Rd(arg1_idx), [f + offsetof(PyFrameObject, f_lasti)]

#if JIT_DEBUG
    // generate code to check that the instruction we jump to had 'is_jmp_target' set
    | mov tmp, arg1
    | shr tmp, 1 // divide by 2, because f_lasti is offset into bytecode array and every bytecode is 2bytes
    | add tmp, ->is_jmp_target
    | cmp byte [tmp], 0
    | jne >9
    | mov arg1, f
    emit_call_ext_func(Dst, debug_error_not_a_jump_target);

    |9:
#endif

    emit_jmp_to_inst_idx(Dst, arg1_idx);
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
            DeferredValueStackEntry* entry = &Dst->deferred_vs[i-1];
            if (entry->loc == CONST) {
                PyObject* obj = PyTuple_GET_ITEM(Dst->co_consts, entry->val);
                emit_mov_imm(Dst, tmp_idx, (unsigned long)obj);
                if (!is_immortal(obj))
                    emit_incref(Dst, tmp_idx);
                | mov [vsp+ 8 * (i-1)], tmp
            } else if (entry->loc == FAST) {
                | mov tmp, [f + get_fastlocal_offset(entry->val)]
                emit_incref(Dst, tmp_idx);
                | mov [vsp+ 8 * (i-1)], tmp
            } else if (entry->loc == REGISTER) {
                | mov [vsp+ 8 * (i-1)], Rq(entry->val)
                if (entry->val == vs_preserved_reg_idx) {
                    emit_mov_imm(Dst, vs_preserved_reg_idx, 0); // we have to clear it because error path will xdecref
                }
            } else if (entry->loc == STACK) {
                | mov tmp, [rsp + (entry->val + NUM_MANUAL_STACK_SLOTS) * 8]
                | mov qword [rsp + (entry->val + NUM_MANUAL_STACK_SLOTS) * 8], 0
                | mov [vsp+ 8 * (i-1)], tmp
            } else {
                JIT_ASSERT(0, "entry->loc not implemented");
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
        Dst->deferred_vs_preserved_reg_used = 0;
        Dst->deferred_vs_res_used = 0;
    }
}

static void deferred_vs_push_no_assert(Jit* Dst, int location, unsigned long value) {
    if (location == REGISTER && value == res_idx && !(ENABLE_DEFERRED_RES_PUSH)) {
        emit_push_v(Dst, res_idx);
    } else {
        if (Dst->deferred_vs_next + 1 >= DEFERRED_VS_MAX) { // make sure we are not writing out of bounds
            deferred_vs_apply(Dst); // TODO: we could just materialize the first stack item instead of all
        }
        GET_DEFERRED[0].loc = location;
        GET_DEFERRED[0].val = value;
        ++Dst->deferred_vs_next;
        if (location == REGISTER && value == res_idx)
            Dst->deferred_vs_res_used = 1;
        if (location == REGISTER && value == vs_preserved_reg_idx)
            Dst->deferred_vs_preserved_reg_used = 1;
    }
}

static void deferred_vs_push(Jit* Dst, int location, unsigned long value) {
    if (location == REGISTER) {
        JIT_ASSERT(value == res_idx, "this is they only registers allowed currently");
    }
    deferred_vs_push_no_assert(Dst, location, value);
}

// returns one of OWNED, BORROWED, or IMMORTAL based on the reference ownership status
static int deferred_vs_peek(Jit* Dst, int r_idx, int num) {
    JIT_ASSERT(num >= 1, "");

    RefStatus ref_status = OWNED;
    if (Dst->deferred_vs_next >= num) {
        int idx = Dst->deferred_vs_next-(num);
        DeferredValueStackEntry* entry = &Dst->deferred_vs[idx];
        if (entry->loc == CONST) {
            PyObject* obj = PyTuple_GET_ITEM(Dst->co_consts, entry->val);
            emit_mov_imm(Dst, r_idx, (unsigned long)obj);
            ref_status = is_immortal(obj) ? IMMORTAL : BORROWED;
        } else if (entry->loc == FAST) {
            | mov Rq(r_idx), [f + get_fastlocal_offset(entry->val)]
            ref_status = BORROWED;
        } else if (entry->loc == REGISTER) {
            // only generate mov if src and dst is different
            if (r_idx != entry->val) {
                | mov Rq(r_idx), Rq(entry->val)
            }
            ref_status = OWNED;
        } else if (entry->loc == STACK) {
            | mov Rq(r_idx), [rsp + (entry->val + NUM_MANUAL_STACK_SLOTS) * 8]
            ref_status = OWNED;
        } else {
            JIT_ASSERT(0, "entry->loc not implemented");
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

// checks if register 'res' is used and if so either moves it to 'preserve_reg2' or to the stack
static void deferred_vs_convert_reg_to_stack(Jit* Dst) {
    if (!Dst->deferred_vs_res_used)
        return; // nothing todo

    for (int i=Dst->deferred_vs_next; i>0; --i) {
        DeferredValueStackEntry* entry = &Dst->deferred_vs[i-1];
        if (entry->loc != REGISTER)
            continue;
        // we only need to handle register 'res' because 'vs_preserved_reg' will not
        // get overwritten by a call.
        if (entry->val != res_idx)
            continue;

        // if we have 'vs_preserved_reg' available use it over a stack slot
        if (!Dst->deferred_vs_preserved_reg_used) {
            | mov vs_preserved_reg, Rq(entry->val)
            entry->loc = REGISTER;
            entry->val = vs_preserved_reg_idx;
            Dst->deferred_vs_preserved_reg_used = 1;
        } else {
            // have to use a stack slot
            if (Dst->num_deferred_stack_slots <= Dst->deferred_stack_slot_next)
                ++Dst->num_deferred_stack_slots;
            | mov [rsp + (Dst->deferred_stack_slot_next + NUM_MANUAL_STACK_SLOTS) * 8], res
            entry->loc = STACK;
            entry->val = Dst->deferred_stack_slot_next;
            ++Dst->deferred_stack_slot_next;
        }
        JIT_ASSERT(Dst->deferred_vs_res_used, "");
        Dst->deferred_vs_res_used = 0;
        break; // finished only reg 'res' needs special handling
    }
}

// removes the top num elements from the value stack
static void deferred_vs_remove(Jit* Dst, int num_to_remove) {
    JIT_ASSERT(num_to_remove >= 0, "");

    if (!num_to_remove)
        return;

    for (int i=0; i < num_to_remove && Dst->deferred_vs_next > i; ++i) {
        DeferredValueStackEntry* entry = &GET_DEFERRED[-i-1];
        if (entry->loc == STACK) {
            | mov qword [rsp + (entry->val + NUM_MANUAL_STACK_SLOTS) * 8], 0
            if (Dst->deferred_stack_slot_next-1 == entry->val)
                --Dst->deferred_stack_slot_next;
        } else if (entry->loc == REGISTER) {
            if (entry->val == vs_preserved_reg_idx) {
                emit_mov_imm(Dst, vs_preserved_reg_idx, 0); // we have to clear it because error path will xdecref
                JIT_ASSERT(Dst->deferred_vs_preserved_reg_used, "should be set");
                Dst->deferred_vs_preserved_reg_used = 0;
            } else if (entry->val == res_idx) {
                JIT_ASSERT(Dst->deferred_vs_res_used, "should be set");
                Dst->deferred_vs_res_used = 0;
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

// pushes a register to the top of the value stack and afterwards calls deferred_vs_apply.
// this allowes the use of any register not only 'res'.
static void deferred_vs_push_reg_and_apply(Jit* Dst, int r_idx) {
    // in this special case (=instant call to deferred_vs_apply) it's safe to
    // use any register so skip the assert.
    deferred_vs_push_no_assert(Dst, REGISTER, r_idx);
    deferred_vs_apply(Dst);
}

// peeks the top stack entry into register r_idx_top and afterwards calls deferred_vs_apply
// generates better code than doing it split
static void deferred_vs_peek_top_and_apply(Jit* Dst, int r_idx_top) {
    JIT_ASSERT(r_idx_top != res_idx && r_idx_top != vs_preserved_reg_idx, "they need special handling");
    if (Dst->deferred_vs_next) {
        // load the value into the destination register and replace the deferred_vs entry
        // with one which accesses the register instead.
        // This is safe because we erase all deferred_vs entries afterwards in deferred_vs_apply.
        // so no additional code needs to know about this register use.
        // Without this we would e.g. generate two memory loads if the top entry is a FAST var access.

        // owned because deferred_vs_apply will consume one ref
        deferred_vs_peek_owned(Dst, r_idx_top, 1 /*=top*/);
        deferred_vs_remove(Dst, 1);
        deferred_vs_push_reg_and_apply(Dst, r_idx_top);
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
        DeferredValueStackEntry* entry = &Dst->deferred_vs[i-1];
        if (entry->loc == FAST && entry->val == var_idx) {
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

static void emit_jump_if_false(Jit* Dst, int oparg, RefStatus ref_status) {
    emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
    emit_je_to_bytecode_n(Dst, oparg);
    emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
    | jne >1

    switch_section(Dst, SECTION_COLD);
    |1:
    emit_call_decref_args1(Dst, PyObject_IsTrue, arg1_idx, &ref_status);
    | cmp res_32b, 0
    emit_je_to_bytecode_n(Dst, oparg);
    | jl ->error
    | jmp >3
    switch_section(Dst, SECTION_CODE);

    |3:
    // continue here
}

static void emit_jump_if_true(Jit* Dst, int oparg, RefStatus ref_status) {
    emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_True);
    emit_je_to_bytecode_n(Dst, oparg);
    emit_cmp_imm(Dst, arg1_idx, (unsigned long)Py_False);
    | jne >1

    switch_section(Dst, SECTION_COLD);
    |1:
    emit_call_decref_args1(Dst, PyObject_IsTrue, arg1_idx, &ref_status);
    | cmp res_32b, 0
    emit_jg_to_bytecode_n(Dst, oparg);
    | jl ->error
    | jmp >3
    switch_section(Dst, SECTION_CODE);

    |3:
    // continue here
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

    // compares tp_version_tag with type_ver
    // branches to false_branch on inequality else continues
    |.macro type_version_check, r_type, type_ver, false_branch
    || // Py_TYPE(obj)->tp_version_tag == type_ver
    |  cmp_imm_mem [r_type + offsetof(PyTypeObject, tp_version_tag)], type_ver
    |  jne false_branch
    |.endmacro

    if (co_opcache == NULL)
        return 1;

    // do we have a valid cache entry?
    if (!co_opcache->optimized)
        return 1;

    if (opcode == LOAD_GLOBAL)  {
        ++jit_stat_load_global_total;
        // The co_opcache->num_failed==0 check is to try to avoid writing out inline
        // caches that might end up missing, since we currently don't rewrite them.
        // It looks like the check is largely useless on our benchmarks, and doesn't
        // meaningfully cut down on the (extremely small) number of cache misses.
        // I think it's still worth leaving it in to reduce potential downside in bad cases,
        // as it definitely helps with the other opcodes.
        // globals_ver != 0 makes sure we don't write out an always-failing inline cache
        if (co_opcache->num_failed == 0 && co_opcache->u.lg.globals_ver != 0) {
            _PyOpcache_LoadGlobal *lg = &co_opcache->u.lg;

            ++jit_stat_load_global_inline;

            deferred_vs_convert_reg_to_stack(Dst);

            | mov arg3, [f + offsetof(PyFrameObject, f_globals)]
            | cmp_imm_mem [arg3 + offsetof(PyDictObject, ma_version_tag)], lg->globals_ver
            | jne >1
            if (lg->builtins_ver != LOADGLOBAL_WAS_GLOBAL) {
                | mov arg3, [f + offsetof(PyFrameObject, f_builtins)]
                | cmp_imm_mem [arg3 + offsetof(PyDictObject, ma_version_tag)], lg->builtins_ver
                | jne >1
            }
            emit_mov_imm(Dst, res_idx, (unsigned long)lg->ptr);
            emit_incref(Dst, res_idx);
            if (jit_stats_enabled) {
                emit_inc_qword_ptr(Dst, &jit_stat_load_global_hit, 1 /*=can use tmp_reg*/);
            }
            |4:
            // fallthrough to next opcode

            // Put the slowpath in a cold section
            switch_section(Dst, SECTION_COLD);
            | 1:
            if (jit_stats_enabled) {
                emit_inc_qword_ptr(Dst, &jit_stat_load_global_miss, 1 /*=can use tmp_reg*/);
            }
            emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                arg2_idx, co_opcache);
            emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, co_opcache != 0 /*= use op cache */));
            emit_if_res_0_error(Dst);
            | jmp <4 // jump to the common code which pushes the result
            // Switch back to the normal section
            switch_section(Dst, SECTION_CODE);

            deferred_vs_push(Dst, REGISTER, res_idx);
            return 0;
        }
    } else if (opcode == LOAD_ATTR || opcode == LOAD_METHOD) {
        if (opcode == LOAD_ATTR)
            ++jit_stat_load_attr_total;
        else
            ++jit_stat_load_method_total;
        if (co_opcache->num_failed == 0 && co_opcache->u.la.type_ver != 0) {
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
                | mov tmp_preserved_reg, arg1

                PyObject* descr = la->u.descr_cache.descr;
                emit_mov_imm(Dst, tmp_idx, (unsigned long)descr);
                | mov arg2, [tmp + offsetof(PyObject, ob_type)]
                | type_version_check, arg2, la->u.descr_cache.descr_type_ver, >1

                // res = descr->ob_type->tp_descr_get(descr, owner, (PyObject *)owner->ob_type);
                | mov arg1, tmp
                | mov arg2, tmp_preserved_reg
                | mov arg3, [tmp_preserved_reg + offsetof(PyObject, ob_type)]
                emit_call_ext_func(Dst, descr->ob_type->tp_descr_get);
                | mov arg1, tmp_preserved_reg // restore the obj so that the decref code works
                // attr can be NULL
                | test res, res
                | jz >3
                emit_load_attr_res_0_helper = 1; // makes sure we emit label 3
            } else if (la->cache_type == LA_CACHE_SLOT_CACHE) {
                // nothing todo
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
            } else if (la->cache_type == LA_CACHE_SLOT_CACHE) {
                | mov res, [arg1 + la->u.slot_cache.offset]
                // attr can be NULL
                | test res, res
                // we can't just jump to label 3 because it would output a different exception message
                // instead jump to the slow path
                | jz >1
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
                emit_inc_qword_ptr(Dst, opcode == LOAD_ATTR ? &jit_stat_load_attr_hit : &jit_stat_load_method_hit, 1 /*=can use tmp_reg*/);
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
            // fallthrough to next opcode

            switch_section(Dst, SECTION_COLD);
            | 1:
            if (jit_stats_enabled) {
                emit_inc_qword_ptr(Dst, opcode == opcode == LOAD_ATTR ? &jit_stat_load_attr_miss : &jit_stat_load_method_miss, 1 /*=can use tmp_reg*/);
            }
            if (opcode == LOAD_ATTR) {
                | mov arg2, arg1
                if (ref_status == BORROWED) { // helper function needs a owned value
                    emit_incref(Dst, arg2_idx);
                }
                emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                    arg3_idx, co_opcache);
            } else {
                emit_mov_imm2(Dst, arg1_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg),
                                    arg2_idx, co_opcache);
            }
            emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, co_opcache != 0 /*= use op cache */));
            emit_if_res_0_error(Dst);
            | jmp <5 // jump to the common code which pushes the result

            if (emit_load_attr_res_0_helper) { // we only emit this code if it's used
                |3:
                | mov tmp_preserved_reg, arg1
                emit_mov_imm(Dst, arg2_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
                emit_call_ext_func(Dst, loadAttrCacheAttrNotFound);
                | mov arg1, tmp_preserved_reg
                | test res, res
                | jnz <4 // jump to the common code which decrefs the obj and pushes the result
                if (ref_status == OWNED) {
                    emit_decref(Dst, tmp_preserved_reg_idx, 0 /*=  don't preserve res */);
                }
                if (jit_stats_enabled) {
                    emit_inc_qword_ptr(Dst, opcode == LOAD_ATTR ? &jit_stat_load_attr_hit : &jit_stat_load_method_hit, 1 /*=can use tmp_reg*/);
                }
                | jmp ->error
            }
            switch_section(Dst, SECTION_CODE);

            deferred_vs_push(Dst, REGISTER, res_idx);
            return 0;
        }
    }
    return 1;
}

static void emit_instr_start(Jit* Dst, int inst_idx, int opcode, int oparg) {
    // The interpreter will only generate a trace line for the first bytecode of line number in the source file.
    // This means that if tracing gets enabled in the middle of a sequence of bytecodes it skips until the start
    // of the next line.
    // Because we don't generate trace checks for some bytecodes we have to manually check
    // if a tracing check is the first for a specific line number even though it may not be the first bytecode
    // for this line.
    // In case it's the first check for a specific line we will overwrite the logic in the interpreter on deopt and
    // force writing out the line trace.
    int current_line_number = PyCode_Addr2Line(Dst->co, inst_idx * 2);
    if (current_line_number != Dst->old_line_number)
        Dst->emitted_trace_check_for_line = 0;
    Dst->old_line_number = current_line_number;

    // we don't emit signal and tracing checks for this opcodes
    // because we know they are not calling into any python function.
    // We currently don't do this optimizations for opcodes like STORE_FAST which
    // could call a destructor.
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
            return;

#if ENABLE_DEFERRED_LOAD_FAST
        case LOAD_FAST:
            if (Dst->known_defined[oparg])
                return; // don't do a sig check if we know the load can't throw
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
    for (int prev_inst_idx = inst_idx-1;
            prev_inst_idx >= 0 && _Py_OPCODE(Dst->first_instr[prev_inst_idx]) == EXTENDED_ARG; --prev_inst_idx)
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
        // compares ceval->tracing_possible == 0 (32bit)
        | cmp dword [interrupt], 0 // inst is 3 bytes long
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
            if (!Dst->emitted_trace_check_for_line) {
                | jmp ->deopt_return_new_line
            } else {
                | jmp ->deopt_return
            }
            switch_section(Dst, SECTION_CODE);
        } else {
            if (!Dst->emitted_trace_check_for_line) {
                | jne ->deopt_return_new_line
            } else {
                | jne ->deopt_return
            }
        }
        break;

    default:
    {
        _Static_assert(offsetof(struct _ceval_runtime_state, tracing_possible) == 4, "cmp need to be modified");
        _Static_assert(offsetof(struct _ceval_runtime_state, eval_breaker) == 8, "cmp need to be modified");
        // compares ceval->tracing_possible == 0 and eval_breaker == 0 in one (64bit)
        | cmp qword [interrupt], 0 // inst is 4 bytes long

        // if we deferred stack operations we have to emit a special deopt path
        if (Dst->deferred_vs_next || num_extended_arg) {
            | jne >1
            switch_section(Dst, SECTION_COLD);
            |1:
            // compares ceval->tracing_possible == 0 (32bit)
            | cmp dword [interrupt], 0
            if (Dst->deferred_vs_res_used) {
                | je ->handle_signal_res_in_use
            } else {
                | je ->handle_signal_res_not_in_use
            }
            deferred_vs_emit(Dst);

            // adjust f_lasti to point to the first EXTENDED_ARG
            if (num_extended_arg) {
                | mov dword [f + offsetof(PyFrameObject, f_lasti)], (inst_idx-num_extended_arg) *2
            }
            if (!Dst->emitted_trace_check_for_line) {
                | jmp ->deopt_return_new_line
            } else {
                | jmp ->deopt_return
            }
            switch_section(Dst, SECTION_CODE);
        } else {
            if (!Dst->emitted_trace_check_for_line) {
                | jne ->handle_tracing_or_signal_no_deferred_stack_new_line
            } else {
                | jne ->handle_tracing_or_signal_no_deferred_stack
            }
        }
        break;
    }
    }
    Dst->emitted_trace_check_for_line = 1;
}

#if JIT_DEBUG
__attribute__((optimize("-O0"))) // enable to make "source tools/dis_jit_gdb.py" work
#endif
void* jit_func(PyCodeObject* co, PyThreadState* tstate) {
    if (mem_bytes_used_max <= mem_bytes_used) // stop emitting code we used up all memory
        return NULL;

    // setup jit context, will get accessed from all dynasm functions via the name 'Dst'
    Jit jit;
    memset(&jit, 0, sizeof(jit));
    jit.co = co;
    jit.co_consts = co->co_consts;
    jit.co_names = co->co_names;
    jit.current_section = -1;

    jit.num_opcodes = PyBytes_Size(co->co_code)/sizeof(_Py_CODEUNIT);
    jit.first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);

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

    // allocate enough space for emitting a dynamic label for the start of every bytecode
    dasm_growpc(Dst,  Dst->num_opcodes + 1);

    jit.is_jmp_target = calculate_jmp_targets(Dst);

    jit.known_defined = (char*)malloc(co->co_nlocals);
    const int funcs_args_are_always_defined = check_func_args_never_deleted(Dst);

    // did we emit the * label already?
    int end_finally_label = 0;
    int deref_error_label = 0;

    Dst->old_line_number = -1;
    Dst->emitted_trace_check_for_line = 0;

    // this is used for the special EXTENDED_ARG opcode
    int oldoparg = 0;
    for (int inst_idx = 0; inst_idx < Dst->num_opcodes; ++inst_idx) {
        _Py_CODEUNIT word = Dst->first_instr[inst_idx];
        int opcode = _Py_OPCODE(word);
        int oparg = _Py_OPARG(word);

        // this is used for the special EXTENDED_ARG opcode
        oparg |= oldoparg;
        oldoparg = 0;

        // if an instruction can jump to this one we need to make sure the deferred stack is clear
        if (Dst->is_jmp_target[inst_idx]) {
            deferred_vs_apply(Dst);
        }

        // if we can jump to this opcode or it's the first in the function
        // we reset the definedness info.
        if (ENABLE_DEFINED_TRACKING && (inst_idx == 0 || Dst->is_jmp_target[inst_idx])) {
            memset(Dst->known_defined, 0, co->co_nlocals);
            for (int i=0; funcs_args_are_always_defined && i<co->co_argcount; ++i) {
                Dst->known_defined[i] = 1; // function arg is defined
            }
        }

        // set jump target for current inst index
        // we can later jump here via =>oparg etc..
        // also used for the opcode_addr table
        |=>inst_idx:

        // emits f->f_lasti update, signal and trace check
        emit_instr_start(Dst, inst_idx, opcode, oparg);

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
            if (!Dst->known_defined[oparg] /* can be null */) {
                | cmp qword [f + get_fastlocal_offset(oparg)], 0
                | je >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_mov_imm(Dst, arg1_idx, oparg); // need to copy it in arg1 because of unboundlocal_error
                | jmp ->unboundlocal_error // arg1 must be oparg!
                switch_section(Dst, SECTION_CODE);

                Dst->known_defined[oparg] = 1;
            }

            deferred_vs_push(Dst, FAST, oparg);
#else
            deferred_vs_apply(Dst);
            if (!Dst->known_defined[oparg] /* can be null */) {
                | mov arg2, [f + get_fastlocal_offset(oparg)]
                | test arg2, arg2
                | jz >1

                switch_section(Dst, SECTION_COLD);
                |1:
                emit_mov_imm(Dst, arg1_idx, oparg); // need to copy it in arg1 because of unboundlocal_error
                | jmp ->unboundlocal_error // arg1 must be oparg!
                switch_section(Dst, SECTION_CODE);

                Dst->known_defined[oparg] = 1;
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
            if (Dst->known_defined[oparg]) {
                emit_decref(Dst, arg1_idx, 0 /* don't preserve res */);
            } else {
                emit_xdecref_arg1(Dst);
            }
            if (ENABLE_DEFINED_TRACKING)
                Dst->known_defined[oparg] = 1;

            break;

        case DELETE_FAST:
        {
            deferred_vs_apply_if_same_var(Dst, oparg);
            | lea tmp, [f + get_fastlocal_offset(oparg)]
            | mov arg2, [tmp]
            if (!Dst->known_defined[oparg] /* can be null */) {
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

            Dst->known_defined[oparg] = 0;

            break;
        }

        case POP_TOP:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            if (ref_status == OWNED) {
                emit_decref(Dst, arg1_idx, Dst->deferred_vs_res_used /*= preserve res */);
            }
            break;
        }

        case ROT_TWO:
            if (Dst->deferred_vs_next >= 2) {
                DeferredValueStackEntry tmp[2];
                memcpy(tmp, &GET_DEFERRED[-2], sizeof(tmp));
                GET_DEFERRED[-1] = tmp[0];
                GET_DEFERRED[-2] = tmp[1];
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
                memcpy(tmp, &GET_DEFERRED[-3], sizeof(tmp));
                GET_DEFERRED[-1] = tmp[1];
                GET_DEFERRED[-2] = tmp[0];
                GET_DEFERRED[-3] = tmp[2];
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
                memcpy(tmp, &GET_DEFERRED[-4], sizeof(tmp));
                GET_DEFERRED[-1] = tmp[2];
                GET_DEFERRED[-2] = tmp[1];
                GET_DEFERRED[-3] = tmp[0];
                GET_DEFERRED[-4] = tmp[3];
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
                (GET_DEFERRED[-1].loc == CONST || GET_DEFERRED[-1].loc == FAST)) {
                GET_DEFERRED[0] = GET_DEFERRED[-1];
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
                (GET_DEFERRED[-1].loc == CONST || GET_DEFERRED[-1].loc == FAST) &&
                (GET_DEFERRED[-2].loc == CONST || GET_DEFERRED[-2].loc == FAST)) {
                GET_DEFERRED[0] = GET_DEFERRED[-2];
                GET_DEFERRED[1] = GET_DEFERRED[-1];
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
            void* func = get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */);
            emit_call_decref_args2(Dst, func, arg2_idx, arg1_idx, ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case POP_JUMP_IF_FALSE:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_apply(Dst);
            emit_jump_if_false(Dst, oparg, ref_status);
            break;
        }

        case POP_JUMP_IF_TRUE:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg1_idx);
            deferred_vs_apply(Dst);
            emit_jump_if_true(Dst, oparg, ref_status);
            break;
        }

        case JUMP_IF_FALSE_OR_POP:
            deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            emit_jump_if_false(Dst, oparg, BORROWED);
            emit_pop_v(Dst, arg1_idx);
            emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
            break;

        case JUMP_IF_TRUE_OR_POP:
            deferred_vs_peek_top_and_apply(Dst, arg1_idx);
            emit_jump_if_true(Dst, oparg, BORROWED);
            emit_pop_v(Dst, arg1_idx);
            emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
            break;

        case CALL_FUNCTION:
        case CALL_METHOD:
            deferred_vs_apply(Dst);
            | mov arg1, tstate

            // arg2 = &sp
            | mov [rsp], vsp
            | mov arg2, rsp

            emit_mov_imm(Dst, arg3_idx, oparg);

            int num_vs_args = oparg + 1;

            if (opcode == CALL_METHOD) {
                num_vs_args += 1;

                // this is taken from clang:
                // meth = PEEK(oparg + 2);
                // arg3 = ((meth == 0) ? 0 : 1) + oparg
                | cmp qword [vsp - (8*num_vs_args)], 1
                | sbb arg3, -1
            }
            emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */));
            emit_adjust_vs(Dst, -num_vs_args);

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
            void* func = get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */);
            emit_call_decref_args1(Dst, func, arg1_idx, &ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case STORE_SUBSCR:
            if (Dst->deferred_vs_next >= 3) {
                RefStatus ref_status[3];
                deferred_vs_pop3(Dst, arg2_idx, arg1_idx, arg3_idx, ref_status);
                deferred_vs_convert_reg_to_stack(Dst);
                void* func = get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */);
                emit_call_decref_args3(Dst, func, arg2_idx, arg1_idx, arg3_idx, ref_status);
                emit_if_res_32b_not_0_error(Dst);
            } else {
                deferred_vs_apply(Dst);
                emit_read_vs(Dst, arg2_idx, 1 /*=top*/);
                emit_read_vs(Dst, arg1_idx, 2 /*=second*/);
                emit_read_vs(Dst, arg3_idx, 3 /*=third*/);
                emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */));
                emit_if_res_32b_not_0_error(Dst);
                for (int i=0; i<3; ++i) {
                    emit_read_vs(Dst, arg1_idx, i+1);
                    emit_decref(Dst, arg1_idx, 0 /*= don't preserve res */);
                }
                emit_adjust_vs(Dst, -3);
            }
            break;

        case DELETE_SUBSCR:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg2_idx, arg1_idx, ref_status);
            deferred_vs_convert_reg_to_stack(Dst);
            void* func = get_aot_func_addr(Dst, opcode, oparg, 0 /*= no op cache */);
            emit_call_decref_args2(Dst, func, arg2_idx, arg1_idx, ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case CALL_FINALLY:
            deferred_vs_apply(Dst);
            // todo: handle during bytecode generation,
            //       this could be normal code object constant entry
            emit_mov_imm(Dst, arg1_idx, (inst_idx+1) * 2);
            emit_call_ext_func(Dst, PyLong_FromLong);
            emit_if_res_0_error(Dst);
            emit_push_v(Dst, res_idx);
            emit_jump_by_n_bytecodes(Dst, oparg, inst_idx);
            break;

        case END_FINALLY:
        {
            RefStatus ref_status = OWNED;
            deferred_vs_pop1_owned(Dst, arg1_idx);
            deferred_vs_apply(Dst);
            | test arg1, arg1
            | jnz ->end_finally

            if (!end_finally_label) {
                end_finally_label = 1;
                switch_section(Dst, SECTION_COLD);
                |->end_finally:
                | cmp dword [arg1 + offsetof(PyObject, ob_type)], (unsigned int)&PyLong_Type
                | jne >2

                // inside CALL_FINALLY we created a long with the bytecode offset to the next instruction
                // extract it and jump to it
                emit_call_decref_args1(Dst, PyLong_AsLong, arg1_idx, &ref_status);
                emit_jmp_to_inst_idx(Dst, res_idx);

                |2:
                | mov arg2, arg1
                | mov arg1, tstate
                emit_read_vs(Dst, arg3_idx, 1 /*=top*/);
                emit_read_vs(Dst, arg4_idx, 2 /*=second*/);
                emit_adjust_vs(Dst, -2);
                emit_call_ext_func(Dst, _PyErr_Restore);
                | jmp ->exception_unwind
                switch_section(Dst, SECTION_CODE);
            }
            break;
        }

        case SET_ADD:
        case LIST_APPEND:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg2_idx);
            deferred_vs_peek(Dst, arg1_idx, oparg);
            deferred_vs_convert_reg_to_stack(Dst);
            void* func = opcode == SET_ADD ? PySet_Add : PyList_Append;
            emit_call_decref_args1(Dst, func, arg2_idx, &ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case MAP_ADD:
        {
            RefStatus ref_status[2];
            deferred_vs_pop2(Dst, arg3_idx, arg2_idx, ref_status);
            deferred_vs_peek(Dst, arg1_idx, oparg);
            deferred_vs_convert_reg_to_stack(Dst);
            emit_call_decref_args2(Dst, PyDict_SetItem, arg3_idx, arg2_idx, ref_status);
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
            emit_call_decref_args2(Dst, import_name, arg4_idx, arg5_idx, ref_status);
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
            emit_call_decref_args1(Dst, PyObject_SetAttr, arg1_idx, &ref_status);
            emit_if_res_32b_not_0_error(Dst);
            break;
        }

        case STORE_GLOBAL:
        {
            RefStatus ref_status = deferred_vs_pop1(Dst, arg3_idx);
            deferred_vs_convert_reg_to_stack(Dst);
            | mov arg1, [f + offsetof(PyFrameObject, f_globals)]
            emit_mov_imm(Dst, arg2_idx, (unsigned long)PyTuple_GET_ITEM(Dst->co_names, oparg));
            emit_call_decref_args1(Dst, PyDict_SetItem, arg3_idx, &ref_status);
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
            emit_call_decref_args3(Dst, PySlice_New, arg3_idx, arg2_idx, arg1_idx, ref_status);
            emit_if_res_0_error(Dst);
            deferred_vs_push(Dst, REGISTER, res_idx);
            break;
        }

        case BUILD_TUPLE:
            // empty tuple optimization
            if (oparg == 0) {
                // todo: handle during bytecode generation
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
            emit_mov_imm(Dst, arg3_idx, oparg); // deref_error assumes that oparg is in arg3!
            | jmp ->deref_error

            if (!deref_error_label) {
                deref_error_label = 1;
                |->deref_error: // assumes that oparg is in arg3!
                | mov arg1, tstate
                emit_mov_imm(Dst, arg2_idx, (unsigned long)co);
                emit_call_ext_func(Dst, format_exc_unbound);
                | jmp ->error
            }

            switch_section(Dst, SECTION_CODE);

            if (opcode == LOAD_DEREF) {
                emit_incref(Dst, res_idx);
                deferred_vs_push(Dst, REGISTER, res_idx);
            } else { // DELETE_DEREF
                | mov qword [arg1 + offsetof(PyCellObject, ob_ref)], 0
                emit_decref(Dst, res_idx, 0 /*= don't preserve res */);
            }
            break;

        case SETUP_FINALLY:
        case SETUP_ASYNC_WITH:
            deferred_vs_apply(Dst);
            // PyFrame_BlockSetup(f, SETUP_FINALLY, INSTR_OFFSET() + oparg, STACK_LEVEL());
            | mov arg1, f
            emit_mov_imm(Dst, arg2_idx, SETUP_FINALLY);
            emit_mov_imm(Dst, arg3_idx, (inst_idx + 1)*2 + oparg);
            // STACK_LEVEL()
            if (opcode == SETUP_ASYNC_WITH) {
                // the interpreter pops the top value and pushes it afterwards
                // we instead just calculate the stack level with the vsp minus one value.
                | lea arg4, [vsp - 8]
            } else {
                | mov arg4, vsp
            }
            | sub arg4, [f + offsetof(PyFrameObject, f_valuestack)]
            | sar arg4, 3 // divide by 8 = sizeof(void*)
            emit_call_ext_func(Dst, PyFrame_BlockSetup);
            break;

        case POP_BLOCK:
            deferred_vs_convert_reg_to_stack(Dst);
            | mov arg1, f
            emit_call_ext_func(Dst, PyFrame_BlockPop);
            break;

        case BEGIN_FINALLY:
            /* Push NULL onto the stack for using it in END_FINALLY,
            POP_FINALLY, WITH_CLEANUP_START and WITH_CLEANUP_FINISH.
            */
            // PUSH(NULL);
            emit_mov_imm(Dst, arg1_idx, 0);
            deferred_vs_push_reg_and_apply(Dst, arg1_idx);
            break;

        default:
            // compiler complains if the first line after a label is a declaration and not a statement:
            (void)0;

            _PyOpcache* co_opcache = NULL;
            if (co->co_opcache != NULL) {
                unsigned char co_opt_offset = co->co_opcache_map[inst_idx + 1];
                if (co_opt_offset > 0) {
                    JIT_ASSERT(co_opt_offset <= co->co_opcache_size, "");
                    co_opcache = &co->co_opcache[co_opt_offset - 1];
                    JIT_ASSERT(co_opcache != NULL, "");
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
                    emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, co_opcache != 0 /*= use op cache */));
                    break;

                case LOAD_ATTR:
                    emit_mov_imm_or_lea(Dst, arg3_idx, arg1_idx, co_opcache, PyTuple_GET_ITEM(Dst->co_names, oparg));
                    emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, co_opcache != 0 /*= use op cache */));
                    break;

                case STORE_ATTR:
                    emit_mov_imm_or_lea(Dst, arg4_idx, arg1_idx, co_opcache, PyTuple_GET_ITEM(Dst->co_names, oparg));
                    emit_call_ext_func(Dst, get_aot_func_addr(Dst, opcode, oparg, co_opcache != 0 /*= use op cache */));
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

    |->handle_signal_res_in_use:
    // we have to preserve res because it's used by our deferred stack optimizations
    | mov tmp_preserved_reg, res
    emit_call_ext_func(Dst, eval_breaker_jit_helper);
    | test res_32b, res_32b
    // on error we have to decref 'res' (which is now in 'tmp_preserved_reg')
    | jnz ->error_decref_tmp_preserved_reg
    // no error, restore 'res' and continue executing
    | mov res, tmp_preserved_reg
    | jmp ->handle_signal_jump_to_inst

    |->handle_signal_res_not_in_use:
    emit_call_ext_func(Dst, eval_breaker_jit_helper);
    emit_if_res_32b_not_0_error(Dst);
    // fall through

    |->handle_signal_jump_to_inst:
    | mov Rd(arg1_idx), [f + offsetof(PyFrameObject, f_lasti)]
    emit_mov_inst_addr(Dst, tmp_idx, arg1_idx);
    // tmp points now to the beginning of the bytecode implementation
    // but we want to skip the signal check.
    // We can't just directly jump after the signal check beause the jne instruction is variable size
    // so instead jump before the conditional jump and set the flags so that we don't jump
    // size of 'mov dword [lasti + offsetof(PyFrameObject, f_lasti)], inst_idx*2' = 8byte
    //       + 'cmp qword [interrupt], 0' = 4byte (64bit cmp)
    // Not that we are in the handle_signal label which can only be reached if we generated
    // the code mentioned above.
    | add tmp, 8 + 4
    | cmp tmp, tmp // dummy to set the flags for 'jne ...' to fail
    | jmp tmp

    |->handle_tracing_or_signal_no_deferred_stack:
    // compares ceval->tracing_possible == 0 (32bit)
    | cmp dword [interrupt], 0
    // there is no deferred stack so we don't have to jump to handle_signal_res_in_use
    | je ->handle_signal_res_not_in_use
    | jmp ->deopt_return

    |->handle_tracing_or_signal_no_deferred_stack_new_line:
    // compares ceval->tracing_possible == 0 (32bit)
    | cmp dword [interrupt], 0
    // there is no deferred stack so we don't have to jump to handle_signal_res_in_use
    | je ->handle_signal_res_not_in_use
    // falltrough

    |->deopt_return_new_line:
    emit_mov_imm(Dst, res_idx, (1 << 2) /* this means first trace check for this line */ | 3 /*= deopt */);
    | jmp ->return

    |->deopt_return:
    emit_mov_imm(Dst, res_idx, 3 /*= deopt */);
    | jmp ->return

    |->error_decref_tmp_preserved_reg:
    emit_decref(Dst, tmp_preserved_reg_idx, 0 /*= don't preserve res */);
    | jmp ->error

    // we come here if the result of LOAD_FAST or DELETE_FAST is null
    |->unboundlocal_error:
    // arg1 must be oparg!
    emit_call_ext_func(Dst, JIT_HELPER_UNBOUNDLOCAL_ERROR);
    // fallthrough to error

    |->error:
    // we have to decref all python object stored in the deferred stack array
    | mov arg1, vs_preserved_reg
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
    | pop tmp_preserved_reg
    | pop vs_preserved_reg
    | ret


    ////////////////////////////////
    // ENTRY OF EMITTED FUNCTION
    switch_section(Dst, SECTION_ENTRY);
    |.align 16
    |->entry:
    // callee saves
    | push vs_preserved_reg
    | push tmp_preserved_reg
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

    // We store the address of _PyRuntime.ceval.tracing_possible and eval_breaker inside a register
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
    emit_mov_imm(Dst, vs_preserved_reg_idx, 0);
    for (int i=0; i<Dst->num_deferred_stack_slots; ++i) {
        | mov qword [rsp + (NUM_MANUAL_STACK_SLOTS + i) * 8], 0
    }

    // jumps either to first opcode implementation or resumes a generator
    emit_jmp_to_lasti(Dst);


    ////////////////////////////////
    // OPCODE TABLE

    // table of bytecode index -> IP
    // used e.g. for continuing generators
    switch_section(Dst, SECTION_OPCODE_ADDR);
    |->opcode_addr_begin:
    for (int inst_idx=0; inst_idx < Dst->num_opcodes; ++inst_idx) {
        // emit 4byte address to start of implementation of instruction with index 'inst_idx'
        // - this is fine our addresses are only 4 byte long not 8
        |.aword =>inst_idx
    }


#if JIT_DEBUG
    |->is_jmp_target:
    for (int i=0; i<Dst->num_opcodes; ++i) {
        |.byte jit.is_jmp_target[i]
    }
#endif

#ifdef DASM_CHECKS
    int dasm_err = dasm_checkstep(Dst, -1);
    if (dasm_err) {
        fprintf(stderr, "dynasm returned error %d", dasm_err);
        JIT_ASSERT(0, "");
    }
#endif

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
        for (int inst_idx = 0; inst_idx < Dst->num_opcodes; ++inst_idx) {
            _Py_CODEUNIT word = Dst->first_instr[inst_idx];
            int opcode = _Py_OPCODE(word);
            int oparg = _Py_OPARG(word);
            void* addr = (void*)(unsigned long)opcode_addr_begin[inst_idx];
            const char* jmp_dst = Dst->is_jmp_target[inst_idx] ? "->" : "  ";
            fprintf(perf_map_opcode_map, "%p,%s %4d %-30s %d\n",
                    addr, jmp_dst, inst_idx*2, get_opcode_name(opcode), oparg);
        }
    }

    dasm_free(Dst);
    free(Dst->is_jmp_target);
    Dst->is_jmp_target = NULL;
    free(Dst->known_defined);
    Dst->known_defined = NULL;

    ++jit_num_funcs;

    return labels[lbl_entry];
}

void show_jit_stats() {
    fprintf(stderr, "jit: compiled %d functions\n", jit_num_funcs);
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
