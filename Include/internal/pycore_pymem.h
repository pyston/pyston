#ifndef Py_INTERNAL_PYMEM_H
#define Py_INTERNAL_PYMEM_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "objimpl.h"
#include "pymem.h"


/* GC runtime state */

/* If we change this, we need to change the default value in the
   signature of gc.collect. */
#define NUM_GENERATIONS 3

/*
   NOTE: about the counting of long-lived objects.

   To limit the cost of garbage collection, there are two strategies;
     - make each collection faster, e.g. by scanning fewer objects
     - do less collections
   This heuristic is about the latter strategy.

   In addition to the various configurable thresholds, we only trigger a
   full collection if the ratio
    long_lived_pending / long_lived_total
   is above a given value (hardwired to 25%).

   The reason is that, while "non-full" collections (i.e., collections of
   the young and middle generations) will always examine roughly the same
   number of objects -- determined by the aforementioned thresholds --,
   the cost of a full collection is proportional to the total number of
   long-lived objects, which is virtually unbounded.

   Indeed, it has been remarked that doing a full collection every
   <constant number> of object creations entails a dramatic performance
   degradation in workloads which consist in creating and storing lots of
   long-lived objects (e.g. building a large list of GC-tracked objects would
   show quadratic performance, instead of linear as expected: see issue #4074).

   Using the above ratio, instead, yields amortized linear performance in
   the total number of objects (the effect of which can be summarized
   thusly: "each full garbage collection is more and more costly as the
   number of objects grows, but we do fewer and fewer of them").

   This heuristic was suggested by Martin von Löwis on python-dev in
   June 2008. His original analysis and proposal can be found at:
    http://mail.python.org/pipermail/python-dev/2008-June/080579.html
*/

/*
   NOTE: about untracking of mutable objects.

   Certain types of container cannot participate in a reference cycle, and
   so do not need to be tracked by the garbage collector. Untracking these
   objects reduces the cost of garbage collections. However, determining
   which objects may be untracked is not free, and the costs must be
   weighed against the benefits for garbage collection.

   There are two possible strategies for when to untrack a container:

   i) When the container is created.
   ii) When the container is examined by the garbage collector.

   Tuples containing only immutable objects (integers, strings etc, and
   recursively, tuples of immutable objects) do not need to be tracked.
   The interpreter creates a large number of tuples, many of which will
   not survive until garbage collection. It is therefore not worthwhile
   to untrack eligible tuples at creation time.

   Instead, all tuples except the empty tuple are tracked when created.
   During garbage collection it is determined whether any surviving tuples
   can be untracked. A tuple can be untracked if all of its contents are
   already not tracked. Tuples are examined for untracking in all garbage
   collection cycles. It may take more than one cycle to untrack a tuple.

   Dictionaries containing only immutable objects also do not need to be
   tracked. Dictionaries are untracked when created. If a tracked item is
   inserted into a dictionary (either as a key or value), the dictionary
   becomes tracked. During a full garbage collection (all generations),
   the collector will untrack any dictionaries whose contents are not
   tracked.

   The module provides the python function is_tracked(obj), which returns
   the CURRENT tracking status of the object. Subsequent garbage
   collections may change the tracking status of the object.

   Untracking of certain containers was introduced in issue #4688, and
   the algorithm was refined in response to issue #14775.
*/

struct gc_generation {
    PyGC_Head head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    /* total number of collections */
    Py_ssize_t collections;
    /* total number of collected objects */
    Py_ssize_t collected;
    /* total number of uncollectable objects (put into gc.garbage) */
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject *trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int trash_delete_nesting;

    int enabled;
    int debug;
    /* linked lists of container objects */
    struct gc_generation generations[NUM_GENERATIONS];
    PyGC_Head *generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int collecting;
    /* list of uncollectable objects */
    PyObject *garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject *callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t long_lived_pending;
};

PyAPI_FUNC(void) _PyGC_Initialize(struct _gc_runtime_state *);


#if PY_DEBUGGING_FEATURES
/* Set the memory allocator of the specified domain to the default.
   Save the old allocator into *old_alloc if it's non-NULL.
   Return on success, or return -1 if the domain is unknown. */
PyAPI_FUNC(int) _PyMem_SetDefaultAllocator(
    PyMemAllocatorDomain domain,
    PyMemAllocatorEx *old_alloc);

/* Special bytes broadcast into debug memory blocks at appropriate times.
   Strings of these are unlikely to be valid addresses, floats, ints or
   7-bit ASCII.

   - PYMEM_CLEANBYTE: clean (newly allocated) memory
   - PYMEM_DEADBYTE dead (newly freed) memory
   - PYMEM_FORBIDDENBYTE: untouchable bytes at each end of a block

   Byte patterns 0xCB, 0xDB and 0xFB have been replaced with 0xCD, 0xDD and
   0xFD to use the same values than Windows CRT debug malloc() and free().
   If modified, _PyMem_IsPtrFreed() should be updated as well. */
#endif
#define PYMEM_CLEANBYTE      0xCD
#define PYMEM_DEADBYTE       0xDD
#define PYMEM_FORBIDDENBYTE  0xFD
#if PY_DEBUGGING_FEATURES

/* Heuristic checking if a pointer value is newly allocated
   (uninitialized), newly freed or NULL (is equal to zero).

   The pointer is not dereferenced, only the pointer value is checked.

   The heuristic relies on the debug hooks on Python memory allocators which
   fills newly allocated memory with CLEANBYTE (0xCD) and newly freed memory
   with DEADBYTE (0xDD). Detect also "untouchable bytes" marked
   with FORBIDDENBYTE (0xFD). */
static inline int _PyMem_IsPtrFreed(void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
#if SIZEOF_VOID_P == 8
    return (value == 0
            || value == (uintptr_t)0xCDCDCDCDCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDDDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFDFDFDFDFD);
#elif SIZEOF_VOID_P == 4
    return (value == 0
            || value == (uintptr_t)0xCDCDCDCD
            || value == (uintptr_t)0xDDDDDDDD
            || value == (uintptr_t)0xFDFDFDFD);
#else
#  error "unknown pointer size"
#endif
}
#endif
#define _PyMem_IsPtrFreed(x) (0)

PyAPI_FUNC(int) _PyMem_GetAllocatorName(
    const char *name,
    PyMemAllocatorName *allocator);

/* Configure the Python memory allocators.
   Pass PYMEM_ALLOCATOR_DEFAULT to use default allocators.
   PYMEM_ALLOCATOR_NOT_SET does nothing. */
PyAPI_FUNC(int) _PyMem_SetupAllocators(PyMemAllocatorName allocator);

/*
 * The system's VMM page size can be obtained on most unices with a
 * getpagesize() call or deduced from various header files. To make
 * things simpler, we assume that it is 4K, which is OK for most systems.
 * It is probably better if this is the native page size, but it doesn't
 * have to be.  In theory, if SYSTEM_PAGE_SIZE is larger than the native page
 * size, then `POOL_ADDR(p)->arenaindex' could rarely cause a segmentation
 * violation fault.  4K is apparently OK for all the platforms that python
 * currently targets.
 */
#define SYSTEM_PAGE_SIZE        (4 * 1024)
#define SYSTEM_PAGE_SIZE_MASK   (SYSTEM_PAGE_SIZE - 1)

/*
 * The allocator sub-allocates <Big> blocks of memory (called arenas) aligned
 * on a page boundary. This is a reserved virtual address space for the
 * current process (obtained through a malloc()/mmap() call). In no way this
 * means that the memory arenas will be used entirely. A malloc(<Big>) is
 * usually an address range reservation for <Big> bytes, unless all pages within
 * this space are referenced subsequently. So malloc'ing big blocks and not
 * using them does not mean "wasting memory". It's an addressable range
 * wastage...
 *
 * Arenas are allocated with mmap() on systems supporting anonymous memory
 * mappings to reduce heap fragmentation.
 */
#define GC_ARENA_SIZE              (256 << 10)     /* 256KB */

/*
 * Size of the pools used for small blocks. Should be a power of 2,
 * between 1K and SYSTEM_PAGE_SIZE, that is: 1k, 2k, 4k.
 */
#define GC_POOL_SIZE               SYSTEM_PAGE_SIZE        /* must be 2^N */
#define GC_POOL_SIZE_MASK          SYSTEM_PAGE_SIZE_MASK

#define MAX_GC_POOLS_IN_ARENA  (GC_ARENA_SIZE / GC_POOL_SIZE)
#if MAX_GC_POOLS_IN_ARENA * GC_POOL_SIZE != GC_ARENA_SIZE
#   error "arena size not an exact multiple of pool size"
#endif

// How many bytes of memory are assigned to a single bitmap location
#define GC_BITMAP_OBJECT_SIZE 16

/* Pool for small blocks. */
struct gc_pool_header {
    union { void *_padding;
            uint count; } ref;          /* number of allocated blocks    */
    void *freeblock;                   /* pool's free list head         */
    struct gc_pool_header *nextusedpool;       /* next pool of this size class  */
    struct gc_pool_header *prevusedpool;       /* previous pool       ""        */
    struct gc_pool_header *nextgenpool;        /* next pool in this generation  */
    struct gc_pool_header *prevgenpool;        /* previous pool       ""        */
    char is_young;              /* 1 if this pool is linked into the youngest generation, 0 if not */
    uint arenaindex;                    /* index into arenas of base adr */
    // This is now either DUMMY (uninitialized) or 0:
    uint szidx;                         /* block size class index        */
    uint nextoffset;                    /* bytes to virgin block         */
    uint maxnextoffset;                 /* largest valid nextoffset      */

    // Bitmap of if block belongs to generation N
    // To keep things simple, the mapping from block to bit in this bitmap
    // is just offset_of_block / GC_BITMAP_OBJECT_SIZE
    // ie it does not depend on the size of the object allocated, and
    // it does not skip the gc_pool_header block.
    // This makes the bitmaps somewhat larger with the benefit of simplicity
    // and runtime speed.
    //
    // Each GC_BITMAP_OBJECT_SIZE-sized block of memory (16 bytes) requires
    // three bits, resulting in a 2% overhead for maintaining these bitmaps.
    // This could be reduced by increasing GC_BITMAP_OBJECT_SIZE to 32, or
    // changing the above design decision, or storing the generation number
    // as a two-bit integer instead of three bit flags.
    long gen_bitmaps[NUM_GENERATIONS][GC_POOL_SIZE / GC_BITMAP_OBJECT_SIZE / (8 * sizeof(long))];
};

typedef struct gc_pool_header *gcpoolp;

typedef struct _allocator {
    size_t nbytes;

    // A doubly-linked list of pools connected by their {next,prev}usedpool members
    gcpoolp usedpools[2];

    // Three doubly-linked lists of pools connected by their {next,prev}genpool members
    gcpoolp generations[NUM_GENERATIONS][2];

    /* Array of objects used to track chunks of memory (arenas). */
    struct arena_object* arenas;
    /* Number of slots currently allocated in the `arenas` vector. */
    uint maxarenas;

    /* The head of the singly-linked, NULL-terminated list of available
     * arena_objects.
     */
    struct arena_object* unused_arena_objects;

    /* The head of the doubly-linked, NULL-terminated at each end, list of
     * arena_objects associated with arenas that have pools available.
     */
    struct arena_object* usable_arenas;

    /* nfp2lasta[nfp] is the last arena in usable_arenas with nfp free pools */
    struct arena_object* nfp2lasta[MAX_GC_POOLS_IN_ARENA + 1];

    /* Number of arenas allocated that haven't been free()'d. */
    size_t narenas_currently_allocated;

    /* Total number of times malloc() called to allocate an arena. */
    size_t ntimes_arena_allocated;
    /* High water mark (max value ever seen) for narenas_currently_allocated. */
    size_t narenas_highwater;

    Py_ssize_t _Py_AllocatedBlocks;

    struct _allocator *next_allocator;

} GCAllocator;

#define GCPOOL_HEAD(alloc, generation)  ((gcpoolp )((uint8_t *)&(alloc->generations[generation][0]) - 4*sizeof(void *)))

// Singly-linked list of allocated GCAllocators, linked via
// the next_allocator field
extern GCAllocator* allocator_list;

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYMEM_H */
