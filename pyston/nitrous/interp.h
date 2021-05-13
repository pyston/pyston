#ifndef _NITROUS_INTERP_H
#define _NITROUS_INTERP_H

#ifdef __cplusplus
extern "C" {
#endif

// HACK: this allows us to define the symbols as weak from inside cpython...
#ifndef DEF
#define DEF
#endif

DEF void initializeJIT(int verbosity, int pic);

DEF void loadBitcode(const char* llvm_filename);

typedef struct _JitTarget {
    void* target_function;
    int num_args;
    unsigned arg_flags;

    void* jitted_trace;

    // JIT2: additions
    void* llvm_jit; // stores the LLVMJit object
    // we decrement this everytime we execute a function, if it reaches 0 we JIT
    int num_traces_until_jit;
    int currently_interpreting;
} JitTarget;

DEF JitTarget* createJitTarget(void* target_function, int num_args, int num_traces_until_jit);
DEF long _runJitTarget(JitTarget* target, ...);

// flag can be JIT_IS_CONST or/and JIT_NOT_ZERO
//   JIT_IS_CONST value will stay the same as on first call
//   JIT_NOT_ZERO value is not null
DEF void setJitTargetArgFlag(JitTarget* target, int arg_num, int flag);

static inline long runJitTarget0(JitTarget* target) {
    if (target->jitted_trace)
        return ((long (*)())target->jitted_trace)();
    return _runJitTarget(target);
}

static inline long runJitTarget1(JitTarget* target, long arg0) {
    if (target->jitted_trace)
        return ((long (*)(long))target->jitted_trace)(arg0);
    return _runJitTarget(target, arg0);
}

static inline long runJitTarget2(JitTarget* target, long arg0, long arg1) {
    if (target->jitted_trace)
        return ((long (*)(long, long))target->jitted_trace)(arg0, arg1);
    return _runJitTarget(target, arg0, arg1);
}

static inline long runJitTarget3(JitTarget* target, long arg0, long arg1, long arg2) {
    if (target->jitted_trace)
        return ((long (*)(long, long, long))target->jitted_trace)(arg0, arg1, arg2);
    return _runJitTarget(target, arg0, arg1, arg2);
}

static inline long runJitTarget4(JitTarget* target, long arg0, long arg1, long arg2, long arg3) {
    if (target->jitted_trace)
        return ((long (*)(long, long, long, long))target->jitted_trace)(arg0, arg1, arg2, arg3);
    return _runJitTarget(target, arg0, arg1, arg2, arg3);
}

static inline long runJitTarget5(JitTarget* target, long arg0, long arg1, long arg2, long arg3, long arg4) {
    if (target->jitted_trace)
        return ((long (*)(long, long, long, long, long))target->jitted_trace)(arg0, arg1, arg2, arg3, arg4);
    return _runJitTarget(target, arg0, arg1, arg2, arg3, arg4);
}

static inline long runJitTarget6(JitTarget* target, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
    if (target->jitted_trace)
        return ((long (*)(long, long, long, long, long, long))target->jitted_trace)(arg0, arg1, arg2, arg3, arg4, arg5);
    return _runJitTarget(target, arg0, arg1, arg2, arg3, arg4, arg5);
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif
