
/* Return the compiler identification, if possible. */

#include "Python.h"

#ifndef COMPILER

#if PYSTON_SPEEDUPS

/* We want COMPILER to expand to a string containing _MSC_VER's *value*.
 * This is horridly tricky, because the stringization operator only works
 * on macro arguments, and doesn't evaluate macros passed *as* arguments.
 * Attempts simpler than the following appear doomed to produce "_MSC_VER"
 * literally in the string.
 */
#define _Py_PASTE_VERSION(SUFFIX) \
        ("MSC v." _Py_STRINGIZE(_MSC_VER) " " SUFFIX)


// Note the __clang__ conditional has to come before the __GNUC__ one because
// clang pretends to be GCC.
#if defined(__clang__)
#define COMPILER "Clang " __clang_version__ ""
#elif defined(__GNUC__)
#define COMPILER "GCC " __VERSION__ ""
#elif defined(__LCC__)
#define COMPILER "LCC Win32"
#elif defined(_MSC_VER)
#ifdef MS_WIN64
#if defined(_M_X64) || defined(_M_AMD64)
#if defined(__INTEL_COMPILER)
#define COMPILER ("ICC v." _Py_STRINGIZE(__INTEL_COMPILER) " 64 bit (amd64) with MSC v." _Py_STRINGIZE(_MSC_VER) " CRT")
#else
#define COMPILER _Py_PASTE_VERSION("64 bit (AMD64)")
#endif /* __INTEL_COMPILER */
#elif defined(_M_ARM64)
#define COMPILER _Py_PASTE_VERSION("64 bit (ARM64)")
#else
#define COMPILER _Py_PASTE_VERSION("64 bit (Unknown)")
#endif /* _M_X64 || _M_AMD64 || _M_ARM64 */
#else /* MS_WIN64 */
#if defined(_M_IX86)
#if defined(__INTEL_COMPILER)
#define COMPILER ("ICC v." _Py_STRINGIZE(__INTEL_COMPILER) " 32 bit (Intel) with MSC v." _Py_STRINGIZE(_MSC_VER) " CRT")
#else
#define COMPILER _Py_PASTE_VERSION("32 bit (Intel)")
#endif /* __INTEL_COMPILER */
#define PYD_PLATFORM_TAG "win32"
#elif defined(_M_ARM)
#define COMPILER _Py_PASTE_VERSION("32 bit (ARM)")
#define PYD_PLATFORM_TAG "win_arm32"
#else
#define COMPILER _Py_PASTE_VERSION("32 bit (Unknown)")
#endif /* _M_IX86 || _M_ARM */
#endif /* MS_WIN64 || MS_WIN32 */
// Generic fallbacks.
#elif defined(__cplusplus)
#define COMPILER "C++"
#else
#define COMPILER "C"
#endif
#else


/* We want COMPILER to expand to a string containing _MSC_VER's *value*.
 * This is horridly tricky, because the stringization operator only works
 * on macro arguments, and doesn't evaluate macros passed *as* arguments.
 * Attempts simpler than the following appear doomed to produce "_MSC_VER"
 * literally in the string.
 */
#define _Py_PASTE_VERSION(SUFFIX) \
        ("[MSC v." _Py_STRINGIZE(_MSC_VER) " " SUFFIX "]")

// Note the __clang__ conditional has to come before the __GNUC__ one because
// clang pretends to be GCC.
#if defined(__clang__)
#define COMPILER "\n[Clang " __clang_version__ "]"
#elif defined(__GNUC__)
#define COMPILER "\n[GCC " __VERSION__ "]"
#elif defined(__LCC__)
#define COMPILER "[lcc-win32]"
#elif defined(_MSC_VER)
#ifdef MS_WIN64
#if defined(_M_X64) || defined(_M_AMD64)
#if defined(__INTEL_COMPILER)
#define COMPILER ("[ICC v." _Py_STRINGIZE(__INTEL_COMPILER) " 64 bit (amd64) with MSC v." _Py_STRINGIZE(_MSC_VER) " CRT]")
#else
#define COMPILER _Py_PASTE_VERSION("64 bit (AMD64)")
#endif /* __INTEL_COMPILER */
#elif defined(_M_ARM64)
#define COMPILER _Py_PASTE_VERSION("64 bit (ARM64)")
#else
#define COMPILER _Py_PASTE_VERSION("64 bit (Unknown)")
#endif /* _M_X64 || _M_AMD64 || _M_ARM64 */
#else /* MS_WIN64 */
#if defined(_M_IX86)
#if defined(__INTEL_COMPILER)
#define COMPILER ("[ICC v." _Py_STRINGIZE(__INTEL_COMPILER) " 32 bit (Intel) with MSC v." _Py_STRINGIZE(_MSC_VER) " CRT]")
#else
#define COMPILER _Py_PASTE_VERSION("32 bit (Intel)")
#endif /* __INTEL_COMPILER */
#define PYD_PLATFORM_TAG "win32"
#elif defined(_M_ARM)
#define COMPILER _Py_PASTE_VERSION("32 bit (ARM)")
#define PYD_PLATFORM_TAG "win_arm32"
#else
#define COMPILER _Py_PASTE_VERSION("32 bit (Unknown)")
#endif /* _M_IX86 || _M_ARM */
#endif /* MS_WIN64 || MS_WIN32 */
// Generic fallbacks.
#elif defined(__cplusplus)
#define COMPILER "[C++]"
#else
#define COMPILER "[C]"
#endif
#endif

#endif /* !COMPILER */

const char *
Py_GetCompiler(void)
{
    return COMPILER;
}
