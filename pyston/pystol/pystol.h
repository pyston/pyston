#ifndef _PYSTOL_PYSTOL_H
#define _PYSTOL_PYSTOL_H

#ifdef __cplusplus
extern "C" {
#endif

struct _object;
typedef struct _object PyObject;
struct _typeobject;
typedef struct _typeobject PyTypeObject;

// HACK: this allows us to define the symbols as weak from inside cpython...
#ifndef DEF
#define DEF
#endif

DEF void pystolGlobalPythonSetup();
DEF void pystolAddConstObj(PyObject* o);
DEF void pystolAddConstType(PyTypeObject* x);

#ifdef __cplusplus
}
#endif

#endif
