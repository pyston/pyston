/* The gcmalloc gc-aware allocator
*/

#ifndef Py_GCMALLOC_H
#define Py_GCMALLOC_H

#include "object.h"

PyAPI_FUNC(void *) new_gcallocator(size_t nbytes, PyTypeObject* type);
PyAPI_FUNC(void *) gcmalloc_alloc(void *alloc);
PyAPI_FUNC(void) gcmalloc_free(void *alloc, void *p);

#endif /* !Py_GCMALLOC_H */
