#ifndef Py_INTERNAL_AOT_CEVAL_JIT_HELPER_H
#define Py_INTERNAL_AOT_CEVAL_JIT_HELPER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"

#if PYSTON_SPEEDUPS

#define JIT_HELPER(name) PyObject* JIT_HELPER_##name(int _not_set)
#define JIT_HELPER1(name, py1) PyObject* JIT_HELPER_##name(int _not_set, PyObject* py1)
#define JIT_HELPER2(name, py1, py2) PyObject* JIT_HELPER_##name(int _not_set, PyObject* py1, PyObject* py2)
#define JIT_HELPER_WITH_OPARG(name) PyObject* JIT_HELPER_##name(int oparg)
#define JIT_HELPER_WITH_OPARG1(name, py1) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1)
#define JIT_HELPER_WITH_OPARG2(name, py1, py2) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1, PyObject* py2)
#define JIT_HELPER_WITH_OPARG3(name, py1, py2, py3) PyObject* JIT_HELPER_##name(int oparg, PyObject* py1, PyObject* py2, PyObject* py3)
#define JIT_HELPER_WITH_NAME(name_) PyObject* JIT_HELPER_##name_(PyObject* name)
#define JIT_HELPER_WITH_NAME1(name_, py1) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT(name_) PyObject* JIT_HELPER_##name_(PyObject* name, _PyOpcache *co_opcache)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT1(name_, py1) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1, _PyOpcache *co_opcache)
#define JIT_HELPER_WITH_NAME_OPCACHE_AOT2(name_, py1, py2) PyObject* JIT_HELPER_##name_(PyObject* name, PyObject* py1, PyObject* py2, _PyOpcache *co_opcache)

// on apple arm64 we can't have a writable and executable page at the same time.
// instead the provide an api to quickly change the protection.
#if __APPLE__ && __aarch64__
#define JIT_MEM_RW() pthread_jit_write_protect_np(0)
#define JIT_MEM_RX() pthread_jit_write_protect_np(1)
#else
#define JIT_MEM_RW()
#define JIT_MEM_RX()
#endif

/* this directly modifies the destination of the jit generated call instruction */\
#if __aarch64__
#define SET_JIT_AOT_FUNC(dst_addr) do { \
    /* retrieve address of the instruction following the call instruction */ \
    unsigned int* ret_addr = (unsigned int*)__builtin_extract_return_addr(__builtin_return_address(0)); \
    JIT_MEM_RW(); \
    if (ret_addr[-1] == 0xD63F00C0 /* blr x6 */ ) { \
        /* we generated one 'mov' followed by 3 'movk' */ \
        ret_addr[-5] = 0xD2800006 | ((unsigned long)dst_addr&0xFFFF)<<5; \
        ret_addr[-4] = 0xF2A00006 | (((unsigned long)dst_addr>>16)&0xFFFF)<<5; \
        ret_addr[-3] = 0xF2C00006 | (((unsigned long)dst_addr>>32)&0xFFFF)<<5; \
        ret_addr[-2] = 0xF2E00006 | (((unsigned long)dst_addr>>48)&0xFFFF)<<5; \
        __builtin___clear_cache(&ret_addr[-5], &ret_addr[-1]); \
    } else { \
        /* this updates the destination of the relative call instruction 'bl' */ \
        ret_addr[-1] = 0x94000000 | (((long)dst_addr - (long)&ret_addr[-1])&((1<<29)-1))>>2; \
        __builtin___clear_cache(&ret_addr[-1], &ret_addr[0]); \
    } \
    JIT_MEM_RX(); \
} while(0)
#else
#define SET_JIT_AOT_FUNC(dst_addr) do { \
    /* retrieve address of the instruction following the call instruction */\
    unsigned char* ret_addr = (unsigned char*)__builtin_extract_return_addr(__builtin_return_address(0));\
    if (ret_addr[-2] == 0xff && ret_addr[-1] == 0xd0) { /* abs call: call rax */\
        unsigned long* call_imm = (unsigned long*)&ret_addr[-2-8];\
        *call_imm = (unsigned long)dst_addr;\
    } else { /* relative call */ \
        /* 5 byte call instruction - get address of relative immediate operand of call */\
        unsigned int* call_imm = (unsigned int*)&ret_addr[-4];\
        /* set operand to newly calculated relative offset */\
        *call_imm = (unsigned int)(unsigned long)(dst_addr) - (unsigned int)(unsigned long)ret_addr;\
    } \
} while(0)
#endif

JIT_HELPER1(PRINT_EXPR, value);
JIT_HELPER_WITH_OPARG(RAISE_VARARGS);
JIT_HELPER1(GET_AITER, obj);
JIT_HELPER(GET_ANEXT);
JIT_HELPER1(GET_AWAITABLE, iterable);
JIT_HELPER1(YIELD_FROM, v);
JIT_HELPER(POP_EXCEPT);
JIT_HELPER_WITH_OPARG(POP_FINALLY);
JIT_HELPER1(END_ASYNC_FOR, exc);
JIT_HELPER(LOAD_BUILD_CLASS);
JIT_HELPER_WITH_NAME1(STORE_NAME, v);
JIT_HELPER_WITH_NAME(DELETE_NAME);
JIT_HELPER_WITH_OPARG1(UNPACK_SEQUENCE, seq);
Py_FLATTEN_FUNCTION JIT_HELPER1(UNPACK_SEQUENCE2, seq);
Py_FLATTEN_FUNCTION JIT_HELPER1(UNPACK_SEQUENCE3, seq);
JIT_HELPER_WITH_OPARG1(UNPACK_EX, seq);
JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR, owner, v);
JIT_HELPER_WITH_NAME_OPCACHE_AOT2(STORE_ATTR_CACHED, owner, v);
JIT_HELPER_WITH_NAME(DELETE_GLOBAL);
JIT_HELPER_WITH_NAME(LOAD_NAME);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_GLOBAL);
JIT_HELPER_WITH_OPARG(UNBOUNDLOCAL_ERROR);
JIT_HELPER_WITH_OPARG(LOAD_CLASSDEREF);
JIT_HELPER_WITH_OPARG(BUILD_STRING);
JIT_HELPER_WITH_OPARG(BUILD_TUPLE_UNPACK_WITH_CALL);
JIT_HELPER_WITH_OPARG(BUILD_TUPLE_UNPACK);
JIT_HELPER_WITH_OPARG(BUILD_LIST_UNPACK);
JIT_HELPER_WITH_OPARG(BUILD_SET);
JIT_HELPER_WITH_OPARG(BUILD_SET_UNPACK);
JIT_HELPER_WITH_OPARG(BUILD_MAP);
JIT_HELPER(SETUP_ANNOTATIONS);
JIT_HELPER_WITH_OPARG(BUILD_CONST_KEY_MAP);
JIT_HELPER_WITH_OPARG(BUILD_MAP_UNPACK);
JIT_HELPER_WITH_OPARG(BUILD_MAP_UNPACK_WITH_CALL);
JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR, owner);
JIT_HELPER_WITH_NAME_OPCACHE_AOT1(LOAD_ATTR_CACHED, owner);
JIT_HELPER1(IMPORT_STAR, from);
JIT_HELPER1(GET_YIELD_FROM_ITER, iterable);
JIT_HELPER(FOR_ITER_SECOND_PART);
JIT_HELPER(BEFORE_ASYNC_WITH);
JIT_HELPER_WITH_OPARG(SETUP_WITH);
JIT_HELPER(WITH_CLEANUP_START);
JIT_HELPER2(WITH_CLEANUP_FINISH, res, exc);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD);
JIT_HELPER_WITH_NAME_OPCACHE_AOT(LOAD_METHOD_CACHED);
JIT_HELPER_WITH_OPARG3(CALL_FUNCTION_EX_internal, kwargs, callargs, func);
Py_FLATTEN_FUNCTION JIT_HELPER_WITH_OPARG2(CALL_FUNCTION_EX_NOKWARGS, callargs, func);
Py_FLATTEN_FUNCTION JIT_HELPER_WITH_OPARG3(CALL_FUNCTION_EX_KWARGS, kwargs, callargs, func);
JIT_HELPER_WITH_OPARG2(MAKE_FUNCTION, qualname, codeobj);
JIT_HELPER_WITH_OPARG(FORMAT_VALUE);

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION == 7
enum why_code;
PyObject* JIT_HELPER_END_FINALLY37(enum why_code* why);
void JIT_HELPER_POP_BLOCK37(void);
#endif

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
JIT_HELPER1(DICT_UPDATE_ERROR, update);
JIT_HELPER2(DICT_MERGE_ERROR, update, func);
JIT_HELPER1(LIST_EXTEND_ERROR, iterable);
JIT_HELPER(WITH_EXCEPT_START);
int JIT_HELPER_EXC_MATCH(PyObject *left, PyObject *right);
#endif

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 10
JIT_HELPER_WITH_OPARG3(RERAISE_OPARG_SET, exc, val, tb);

JIT_HELPER1(GET_LEN, top);
JIT_HELPER_WITH_OPARG(MATCH_CLASS);
JIT_HELPER1(MATCH_MAPPING, subject);
JIT_HELPER1(MATCH_SEQUENCE, subject);
JIT_HELPER(MATCH_KEYS);
JIT_HELPER(COPY_DICT_WITHOUT_KEYS);
JIT_HELPER_WITH_OPARG(ROT_N);
#endif

#endif

#ifdef __cplusplus
}
#endif
#endif
