#define _PyErr_SetString(tstate, exception, string) PyErr_SetString(exception, string)
#define _PyErr_Occurred(tstate) PyErr_Occurred()
#define _PyThreadState_Swap(gilstate, tstate) PyThreadState_Swap(tstate)
#define _PyErr_Clear(tstate) PyErr_Clear()
#define _PyErr_SetObject(tstate, exc, obj) PyErr_SetObject(exc, obj)
#define _PyErr_SetNone(tstate, exc) PyErr_SetNone(exc)
#define _PyErr_Format(tstate, exc, format, ...) PyErr_Format(exc, format, ##__VA_ARGS__)
#define _PyErr_ExceptionMatches(tstate, exc) PyErr_ExceptionMatches(exc)
#define _PyErr_Restore(tstate, exc, val, tb) PyErr_Restore(exc, val, tb)
#define _PyErr_Fetch(tstate, exc, val, tb) PyErr_Fetch(exc, val, tb)
#define _PyErr_NormalizeException(tstate, exc, val, tb) PyErr_NormalizeException(exc, val, tb)

/* Status code for main loop (reason for stack unwind) */
enum why_code {
        WHY_NOT =       0x0001, /* No error */
        WHY_EXCEPTION = 0x0002, /* Exception occurred */
        WHY_RETURN =    0x0008, /* 'return' statement */
        WHY_BREAK =     0x0010, /* 'break' statement */
        WHY_CONTINUE =  0x0020, /* 'continue' statement */
        WHY_YIELD =     0x0040, /* 'yield' operator */
        WHY_SILENCED =  0x0080  /* Exception silenced by 'with' */
};

