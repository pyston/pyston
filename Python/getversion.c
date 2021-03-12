
/* Return the full version string. */

#include "Python.h"

#include "patchlevel.h"

const char *
Py_GetVersion(void)
{
    static char version[250];
#if PYSTON_SPEEDUPS
    PyOS_snprintf(version, sizeof(version), "%.80s (%.80s)\n[Pyston %d.%d.%d, %.80s]",
                  PY_VERSION, Py_GetBuildInfo(), PYSTON_MAJOR_VERSION,
                  PYSTON_MINOR_VERSION, PYSTON_MICRO_VERSION, Py_GetCompiler());
#else
    PyOS_snprintf(version, sizeof(version), "%.80s (%.80s) %.80s",
                  PY_VERSION, Py_GetBuildInfo(), Py_GetCompiler());
#endif
    return version;
}
