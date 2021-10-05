#include "Python.h"
#include "pycore_pymem.h"

#include <stdbool.h>

/*
   A gc-optimized memory allocator, based on pymalloc.
   The code is copied from obmalloc.c and modified.

   Compared to pymalloc the main changes are:
   - Add the ability to separate objects by type
   - Add the ability to quickly enumerate live objects by type and generation

   There are also more minor changes:
   - No fallback to the system allocator, skipping some checks.
     This is because this is a special-purpose allocator that
     has to be opted into at compile time for fixed-size objects.
   - This allocator adds a PyGC_HEAD header, since from the pov
     of the client the exact gc mechanism is an implementation detail.

   The first change is done by refactoring the code so that the
   allocator state is no longer global.

   The second is by linking pools into linked lists based on the lowest
   generation number of any of their contained objects. This way we can
   quickly enumerate any pools with any objects in generation 0/1/2.

   To find the objects within a pool, we maintain three bitmaps, one per
   generation. An entry in the bitmap is set if there is a live object
   at that address that belongs to the corresponding generation.
*/

/* Get an object's GC head */
#define AS_GC(o) ((PyGC_Head *)(o)-1)

/* Get the object given the GC head */
#define FROM_GC(g) ((PyObject *)(((PyGC_Head *)g)+1))

void * _PyObject_ArenaMmap(void *ctx, size_t size);
void _PyObject_ArenaMunmap(void *ctx, void *ptr, size_t size);

/* An object allocator for Python.

   Here is an introduction to the layers of the Python memory architecture,
   showing where the object allocator is actually used (layer +2), It is
   called for every object allocation and deallocation (PyObject_New/Del),
   unless the object-specific allocators implement a proprietary allocation
   scheme (ex.: ints use a simple free list). This is also the place where
   the cyclic garbage collector operates selectively on container objects.


    Object-specific allocators
    _____   ______   ______       ________
   [ int ] [ dict ] [ list ] ... [ string ]       Python core         |
+3 | <----- Object-specific memory -----> | <-- Non-object memory --> |
    _______________________________       |                           |
   [   Python's object allocator   ]      |                           |
+2 | ####### Object memory ####### | <------ Internal buffers ------> |
    ______________________________________________________________    |
   [          Python's raw memory allocator (PyMem_ API)          ]   |
+1 | <----- Python memory (under PyMem manager's control) ------> |   |
    __________________________________________________________________
   [    Underlying general-purpose allocator (ex: C library malloc)   ]
 0 | <------ Virtual memory allocated for the python process -------> |

   =========================================================================
    _______________________________________________________________________
   [                OS-specific Virtual Memory Manager (VMM)               ]
-1 | <--- Kernel dynamic storage allocation & management (page-based) ---> |
    __________________________________   __________________________________
   [                                  ] [                                  ]
-2 | <-- Physical memory: ROM/RAM --> | | <-- Secondary storage (swap) --> |

*/
/*==========================================================================*/

/* A fast, special-purpose memory allocator for small blocks, to be used
   on top of a general-purpose malloc -- heavily based on previous art. */

/* Vladimir Marangozov -- August 2000 */

/*
 * "Memory management is where the rubber meets the road -- if we do the wrong
 * thing at any level, the results will not be good. And if we don't make the
 * levels work well together, we are in serious trouble." (1)
 *
 * (1) Paul R. Wilson, Mark S. Johnstone, Michael Neely, and David Boles,
 *    "Dynamic Storage Allocation: A Survey and Critical Review",
 *    in Proc. 1995 Int'l. Workshop on Memory Management, September 1995.
 */

/* #undef WITH_MEMORY_LIMITS */         /* disable mem limit checks  */

/*==========================================================================*/

/*
 * Allocation strategy abstract:
 *
 * For small requests, the allocator sub-allocates <Big> blocks of memory.
 * Requests greater than SMALL_REQUEST_THRESHOLD bytes are routed to the
 * system's allocator.
 *
 * Small requests are grouped in size classes spaced 8 bytes apart, due
 * to the required valid alignment of the returned address. Requests of
 * a particular size are serviced from memory pools of 4K (one VMM page).
 * Pools are fragmented on demand and contain free lists of blocks of one
 * particular size class. In other words, there is a fixed-size allocator
 * for each size class. Free pools are shared by the different allocators
 * thus minimizing the space reserved for a particular size class.
 *
 * This allocation strategy is a variant of what is known as "simple
 * segregated storage based on array of free lists". The main drawback of
 * simple segregated storage is that we might end up with lot of reserved
 * memory for the different free lists, which degenerate in time. To avoid
 * this, we partition each free list in pools and we share dynamically the
 * reserved space between all free lists. This technique is quite efficient
 * for memory intensive programs which allocate mainly small-sized blocks.
 *
 * For small requests we have the following table:
 *
 * Request in bytes     Size of allocated block      Size class idx
 * ----------------------------------------------------------------
 *        1-8                     8                       0
 *        9-16                   16                       1
 *       17-24                   24                       2
 *       25-32                   32                       3
 *       33-40                   40                       4
 *       41-48                   48                       5
 *       49-56                   56                       6
 *       57-64                   64                       7
 *       65-72                   72                       8
 *        ...                   ...                     ...
 *      497-504                 504                      62
 *      505-512                 512                      63
 *
 *      0, SMALL_REQUEST_THRESHOLD + 1 and up: routed to the underlying
 *      allocator.
 */

/*==========================================================================*/

/*
 * -- Main tunable settings section --
 */

/*
 * Alignment of addresses returned to the user. 8-bytes alignment works
 * on most current architectures (with 32-bit or 64-bit address busses).
 * The alignment value is also used for grouping small requests in size
 * classes spaced ALIGNMENT bytes apart.
 *
 * You shouldn't change this unless you know what you are doing.
 */

#if SIZEOF_VOID_P > 4
#define ALIGNMENT              16               /* must be 2^N */
#define ALIGNMENT_SHIFT         4
#else
#define ALIGNMENT               8               /* must be 2^N */
#define ALIGNMENT_SHIFT         3
#endif

/*
 * Max size threshold below which malloc requests are considered to be
 * small enough in order to use preallocated memory pools. You can tune
 * this value according to your application behaviour and memory needs.
 *
 * Note: a size threshold of 512 guarantees that newly created dictionaries
 * will be allocated from preallocated memory pools on 64-bit.
 *
 * The following invariants must hold:
 *      1) ALIGNMENT <= SMALL_REQUEST_THRESHOLD <= 512
 *      2) SMALL_REQUEST_THRESHOLD is evenly divisible by ALIGNMENT
 *
 * Although not required, for better performance and space efficiency,
 * it is recommended that SMALL_REQUEST_THRESHOLD is set to a power of 2.
 */
#define SMALL_REQUEST_THRESHOLD 512
#define NB_SMALL_SIZE_CLASSES   (SMALL_REQUEST_THRESHOLD / ALIGNMENT)


/*
 * Maximum amount of memory managed by the allocator for small requests.
 */
#ifdef WITH_MEMORY_LIMITS
#ifndef SMALL_MEMORY_LIMIT
#define SMALL_MEMORY_LIMIT      (64 * 1024 * 1024)      /* 64 MB -- more? */
#endif
#endif

#ifdef WITH_MEMORY_LIMITS
#define MAX_ARENAS              (SMALL_MEMORY_LIMIT / GC_ARENA_SIZE)
#endif

/*
 * -- End of tunable settings section --
 */

/*==========================================================================*/

/* When you say memory, my mind reasons in terms of (pointers to) blocks */
typedef uint8_t block;

typedef struct gc_pool_header *poolp;

/* Record keeping for arenas. */
struct arena_object {
    /* The address of the arena, as returned by malloc.  Note that 0
     * will never be returned by a successful malloc, and is used
     * here to mark an arena_object that doesn't correspond to an
     * allocated arena.
     */
    uintptr_t address;

    /* Pool-aligned pointer to the next pool to be carved off. */
    block* pool_address;

    /* The number of available pools in the arena:  free pools + never-
     * allocated pools.
     */
    uint nfreepools;

    /* The total number of pools in the arena, whether or not available. */
    uint ntotalpools;

    /* Singly-linked list of available pools. */
    struct gc_pool_header* freepools;

    /* Whenever this arena_object is not associated with an allocated
     * arena, the nextarena member is used to link all unassociated
     * arena_objects in the singly-linked `unused_arena_objects` list.
     * The prevarena member is unused in this case.
     *
     * When this arena_object is associated with an allocated arena
     * with at least one available pool, both members are used in the
     * doubly-linked `usable_arenas` list, which is maintained in
     * increasing order of `nfreepools` values.
     *
     * Else this arena_object is associated with an allocated arena
     * all of whose pools are in use.  `nextarena` and `prevarena`
     * are both meaningless in this case.
     */
    struct arena_object* nextarena;
    struct arena_object* prevarena;
};

#define POOL_OVERHEAD   _Py_SIZE_ROUND_UP(sizeof(struct gc_pool_header), ALIGNMENT)

#define DUMMY_SIZE_IDX          0xffff  /* size class of newly cached pools */

/* Round pointer P down to the closest pool-aligned address <= P, as a poolp */
#define POOL_ADDR(P) ((poolp)_Py_ALIGN_DOWN((P), GC_POOL_SIZE))

/* Return total number of blocks in pool of size index I, as a uint. */
#define NUMBLOCKS(I) ((uint)(GC_POOL_SIZE - POOL_OVERHEAD) / INDEX2SIZE(I))

/*==========================================================================*/

/*
 * Pool table -- headed, circular, doubly-linked lists of partially used pools.

This is involved.  For an index i, usedpools[i+i] is the header for a list of
all partially used pools holding small blocks with "size class idx" i. So
usedpools[0] corresponds to blocks of size 8, usedpools[2] to blocks of size
16, and so on:  index 2*i <-> blocks of size (i+1)<<ALIGNMENT_SHIFT.

Pools are carved off an arena's highwater mark (an arena_object's pool_address
member) as needed.  Once carved off, a pool is in one of three states forever
after:

used == partially used, neither empty nor full
    At least one block in the pool is currently allocated, and at least one
    block in the pool is not currently allocated (note this implies a pool
    has room for at least two blocks).
    This is a pool's initial state, as a pool is created only when malloc
    needs space.
    The pool holds blocks of a fixed size, and is in the circular list headed
    at usedpools[i] (see above).  It's linked to the other used pools of the
    same size class via the gc_pool_header's nextusedpool and prevusedpool members.
    If all but one block is currently allocated, a malloc can cause a
    transition to the full state.  If all but one block is not currently
    allocated, a free can cause a transition to the empty state.

full == all the pool's blocks are currently allocated
    On transition to full, a pool is unlinked from its usedpools[] list.
    It's not linked to from anything then anymore, and its nextusedpool and
    prevusedpool members are meaningless until it transitions back to used.
    A free of a block in a full pool puts the pool back in the used state.
    Then it's linked in at the front of the appropriate usedpools[] list, so
    that the next allocation for its size class will reuse the freed block.

empty == all the pool's blocks are currently available for allocation
    On transition to empty, a pool is unlinked from its usedpools[] list,
    and linked to the front of its arena_object's singly-linked freepools list,
    via its nextusedpool member.  The prevusedpool member has no meaning in this case.
    Empty pools have no inherent size class:  the next time a malloc finds
    an empty list in usedpools[], it takes the first pool off of freepools.
    If the size class needed happens to be the same as the size class the pool
    last had, some pool initialization can be skipped.


Block Management

Blocks within pools are again carved out as needed.  pool->freeblock points to
the start of a singly-linked list of free blocks within the pool.  When a
block is freed, it's inserted at the front of its pool's freeblock list.  Note
that the available blocks in a pool are *not* linked all together when a pool
is initialized.  Instead only "the first two" (lowest addresses) blocks are
set up, returning the first such block, and setting pool->freeblock to a
one-block list holding the second such block.  This is consistent with that
pymalloc strives at all levels (arena, pool, and block) never to touch a piece
of memory until it's actually needed.

So long as a pool is in the used state, we're certain there *is* a block
available for allocating, and pool->freeblock is not NULL.  If pool->freeblock
points to the end of the free list before we've carved the entire pool into
blocks, that means we simply haven't yet gotten to one of the higher-address
blocks.  The offset from the gc_pool_header to the start of "the next" virgin
block is stored in the gc_pool_header nextoffset member, and the largest value
of nextoffset that makes sense is stored in the maxnextoffset member when a
pool is initialized.  All the blocks in a pool have been passed out at least
once when and only when nextoffset > maxnextoffset.


Major obscurity:  While the usedpools vector is declared to have poolp
entries, it doesn't really.  It really contains two pointers per (conceptual)
poolp entry, the nextusedpool and prevusedpool members of a gc_pool_header.  The
excruciating initialization code below fools C so that

    usedpool[i+i]

"acts like" a genuine poolp, but only so long as you only reference its
nextusedpool and prevusedpool members.  The "- 2*sizeof(block *)" gibberish is
compensating for that a gc_pool_header's nextusedpool and prevusedpool members
immediately follow a gc_pool_header's first two members:

    union { block *_padding;
            uint count; } ref;
    block *freeblock;

each of which consume sizeof(block *) bytes.  So what usedpools[i+i] really
contains is a fudged-up pointer p such that *if* C believes it's a poolp
pointer, then p->nextusedpool and p->prevusedpool are both p (meaning that the headed
circular list is empty).

It's unclear why the usedpools setup is so convoluted.  It could be to
minimize the amount of cache required to hold this heavily-referenced table
(which only *needs* the two interpool pointer members of a gc_pool_header). OTOH,
referencing code has to remember to "double the index" and doing so isn't
free, usedpools[0] isn't a strictly legal pointer, and we're crucially relying
on that C doesn't insert any padding anywhere in a gc_pool_header at or before
the prevusedpool member.
**************************************************************************** */

/*==========================================================================
Arena management.

`arenas` is a vector of arena_objects.  It contains maxarenas entries, some of
which may not be currently used (== they're arena_objects that aren't
currently associated with an allocated arena).  Note that arenas proper are
separately malloc'ed.

Prior to Python 2.5, arenas were never free()'ed.  Starting with Python 2.5,
we do try to free() arenas, and use some mild heuristic strategies to increase
the likelihood that arenas eventually can be freed.

unused_arena_objects

    This is a singly-linked list of the arena_objects that are currently not
    being used (no arena is associated with them).  Objects are taken off the
    head of the list in new_arena(), and are pushed on the head of the list in
    PyObject_Free() when the arena is empty.  Key invariant:  an arena_object
    is on this list if and only if its .address member is 0.

usable_arenas

    This is a doubly-linked list of the arena_objects associated with arenas
    that have pools available.  These pools are either waiting to be reused,
    or have not been used before.  The list is sorted to have the most-
    allocated arenas first (ascending order based on the nfreepools member).
    This means that the next allocation will come from a heavily used arena,
    which gives the nearly empty arenas a chance to be returned to the system.
    In my unscientific tests this dramatically improved the number of arenas
    that could be freed.

Note that an arena_object associated with an arena all of whose pools are
currently in use isn't on either list.

Changed in Python 3.8:  keeping usable_arenas sorted by number of free pools
used to be done by one-at-a-time linear search when an arena's number of
free pools changed.  That could, overall, consume time quadratic in the
number of arenas.  That didn't really matter when there were only a few
hundred arenas (typical!), but could be a timing disaster when there were
hundreds of thousands.  See bpo-37029.

Now we have a vector of "search fingers" to eliminate the need to search:
nfp2lasta[nfp] returns the last ("rightmost") arena in usable_arenas
with nfp free pools.  This is NULL if and only if there is no arena with
nfp free pools in usable_arenas.
*/

/*
 nptrs: the number of pointer-sized words that the linked-list entries
 are offset into the gc_pool_header object
*/
#define PTA(llhead, x, nptrs)  ((poolp )((uint8_t *)&(llhead[2*(x)]) - nptrs*sizeof(block *)))

GCAllocator*
new_gcallocator(size_t nbytes, PyTypeObject* type) {
    nbytes += sizeof(PyGC_Head);
    nbytes = _Py_SIZE_ROUND_UP(nbytes, GC_BITMAP_OBJECT_SIZE);

    assert(nbytes <= SMALL_REQUEST_THRESHOLD);

    GCAllocator* alloc = (GCAllocator*)calloc(1, sizeof(GCAllocator));

    alloc->nbytes = nbytes;
    alloc->usedpools[0] = PTA(alloc->usedpools, 0, 2);
    alloc->usedpools[1] = PTA(alloc->usedpools, 0, 2);

    for (int i = 0; i < NUM_GENERATIONS; i++) {
        alloc->generations[i][0] = PTA(alloc->generations[i], 0, 4);
        alloc->generations[i][1] = PTA(alloc->generations[i], 0, 4);
    }

    return alloc;
}

/* How many arena_objects do we initially allocate?
 * 16 = can allocate 16 arenas = 16 * GC_ARENA_SIZE = 4MB before growing the
 * `arenas` vector.
 */
#define INITIAL_ARENA_OBJECTS 16

/* Allocate a new arena.  If we run out of memory, return NULL.  Else
 * allocate a new arena, and return the address of an arena_object
 * describing the new arena.  It's expected that the caller will set
 * `usable_arenas` to the return value.
 */
static struct arena_object*
new_arena(GCAllocator* alloc)
{
    struct arena_object* arenaobj;
    uint excess;        /* number of bytes above pool alignment */
    void *address;

#if 0
    static int debug_stats = -1;
    if (debug_stats == -1) {
        const char *opt = Py_GETENV("PYTHONMALLOCSTATS");
        debug_stats = (opt != NULL && *opt != '\0');
    }
    if (debug_stats)
        _PyObject_DebugMallocStats(stderr);
#endif

    if (alloc->unused_arena_objects == NULL) {
        uint i;
        uint numarenas;
        size_t nbytes;

        /* Double the number of arena objects on each allocation.
         * Note that it's possible for `numarenas` to overflow.
         */
        numarenas = alloc->maxarenas ? alloc->maxarenas << 1 : INITIAL_ARENA_OBJECTS;
        if (numarenas <= alloc->maxarenas)
            return NULL;                /* overflow */
#if SIZEOF_SIZE_T <= SIZEOF_INT
        if (numarenas > SIZE_MAX / sizeof(*arenas))
            return NULL;                /* overflow */
#endif
        nbytes = numarenas * sizeof(*alloc->arenas);
        arenaobj = (struct arena_object *)PyMem_RawRealloc(alloc->arenas, nbytes);
        if (arenaobj == NULL)
            return NULL;
        alloc->arenas = arenaobj;

        /* We might need to fix pointers that were copied.  However,
         * new_arena only gets called when all the pages in the
         * previous arenas are full.  Thus, there are *no* pointers
         * into the old array. Thus, we don't have to worry about
         * invalid pointers.  Just to be sure, some asserts:
         */
        assert(alloc->usable_arenas == NULL);
        assert(alloc->unused_arena_objects == NULL);

        /* Put the new arenas on the unused_arena_objects list. */
        for (i = alloc->maxarenas; i < numarenas; ++i) {
            alloc->arenas[i].address = 0;              /* mark as unassociated */
            alloc->arenas[i].nextarena = i < numarenas - 1 ?
                                       &alloc->arenas[i+1] : NULL;
        }

        /* Update globals. */
        alloc->unused_arena_objects = &alloc->arenas[alloc->maxarenas];
        alloc->maxarenas = numarenas;
    }

    /* Take the next available arena object off the head of the list. */
    assert(alloc->unused_arena_objects != NULL);
    arenaobj = alloc->unused_arena_objects;
    alloc->unused_arena_objects = arenaobj->nextarena;
    assert(arenaobj->address == 0);
#if PY_DEBUGGING_FEATURES
    address = _PyObject_Arena.alloc(_PyObject_Arena.ctx, GC_ARENA_SIZE);
#else
    address = _PyObject_ArenaMmap(NULL, GC_ARENA_SIZE);
#endif
    if (address == NULL) {
        /* The allocation failed: return NULL after putting the
         * arenaobj back.
         */
        arenaobj->nextarena = alloc->unused_arena_objects;
        alloc->unused_arena_objects = arenaobj;
        return NULL;
    }
    arenaobj->address = (uintptr_t)address;

    ++alloc->narenas_currently_allocated;
    ++alloc->ntimes_arena_allocated;
    if (alloc->narenas_currently_allocated > alloc->narenas_highwater)
        alloc->narenas_highwater = alloc->narenas_currently_allocated;
    arenaobj->freepools = NULL;
    /* pool_address <- first pool-aligned address in the arena
       nfreepools <- number of whole pools that fit after alignment */
    arenaobj->pool_address = (block*)arenaobj->address;
    arenaobj->nfreepools = MAX_GC_POOLS_IN_ARENA;
    excess = (uint)(arenaobj->address & GC_POOL_SIZE_MASK);
    if (excess != 0) {
        --arenaobj->nfreepools;
        arenaobj->pool_address += GC_POOL_SIZE - excess;
    }
    arenaobj->ntotalpools = arenaobj->nfreepools;

    return arenaobj;
}

set_bitmap(poolp pool, block *bp) {
    int idx = ((char*)bp - (char*)pool) / GC_BITMAP_OBJECT_SIZE;

    long* ptr = pool->gen_bitmaps[0] + (idx / (8 * sizeof(long)));
    *ptr |= 1 << (idx % (8 * sizeof(long)));
}


/*==========================================================================*/

/* gcmalloc allocator

   The basic blocks are ordered by decreasing execution frequency,
   which minimizes the number of jumps in the most common cases,
   improves branching prediction and instruction scheduling (small
   block allocations typically result in a couple of instructions).
   Unless the optimizer reorders everything, being too smart...

   Return a pointer to newly allocated memory if gcmalloc allocated memory.

   Return NULL if gcmalloc failed to allocate the memory block: on bigger
   requests, on error in the code below (as a last chance to serve the request)
   or when the max memory limit has been reached. */
void*
gcmalloc_alloc(GCAllocator *alloc)
{
    block *bp;
    poolp pool;
    poolp next;
    poolp prev;
    uint size;

#ifdef WITH_VALGRIND
    if (UNLIKELY(running_on_valgrind == -1)) {
        running_on_valgrind = RUNNING_ON_VALGRIND;
    }
    if (UNLIKELY(running_on_valgrind)) {
        return NULL;
    }
#endif

    /*
     * Most frequent paths first
     */
    pool = alloc->usedpools[0];
    if (pool != pool->nextusedpool) {
        /*
         * There is a used pool for this size class.
         * Pick up the head block of its free list.
         */
        ++pool->ref.count;
        bp = pool->freeblock;
        assert(bp != NULL);
        if ((pool->freeblock = *(block **)bp) != NULL) {
            goto success;
        }

        /*
         * Reached the end of the free list, try to extend it.
         */
        if (pool->nextoffset <= pool->maxnextoffset) {
            /* There is room for another block. */
            pool->freeblock = (block*)pool +
                              pool->nextoffset;
            pool->nextoffset += alloc->nbytes;
            *(block **)(pool->freeblock) = NULL;
            goto success;
        }

        /* Pool is full, unlink from used pools. */
        next = pool->nextusedpool;
        prev = pool->prevusedpool;
        next->prevusedpool = prev;
        prev->nextusedpool = next;
        goto success;
    }

    /* There isn't a pool of the right size class immediately
     * available:  use a free pool.
     */
    if (alloc->usable_arenas == NULL) {
        /* No arena has a free pool:  allocate a new arena. */
#ifdef WITH_MEMORY_LIMITS
        if (narenas_currently_allocated >= MAX_ARENAS) {
            goto failed;
        }
#endif
        alloc->usable_arenas = new_arena(alloc);
        if (alloc->usable_arenas == NULL) {
            goto failed;
        }
        alloc->usable_arenas->nextarena =
            alloc->usable_arenas->prevarena = NULL;
        assert(nfp2lasta[alloc->usable_arenas->nfreepools] == NULL);
        alloc->nfp2lasta[alloc->usable_arenas->nfreepools] = alloc->usable_arenas;
    }
    assert(alloc->usable_arenas->address != 0);

    /* This arena already had the smallest nfreepools value, so decreasing
     * nfreepools doesn't change that, and we don't need to rearrange the
     * usable_arenas list.  However, if the arena becomes wholly allocated,
     * we need to remove its arena_object from usable_arenas.
     */
    assert(alloc->usable_arenas->nfreepools > 0);
    if (alloc->nfp2lasta[alloc->usable_arenas->nfreepools] == alloc->usable_arenas) {
        /* It's the last of this size, so there won't be any. */
        alloc->nfp2lasta[alloc->usable_arenas->nfreepools] = NULL;
    }
    /* If any free pools will remain, it will be the new smallest. */
    if (alloc->usable_arenas->nfreepools > 1) {
        assert(alloc->nfp2lasta[alloc->usable_arenas->nfreepools - 1] == NULL);
        alloc->nfp2lasta[alloc->usable_arenas->nfreepools - 1] = alloc->usable_arenas;
    }

    /* Try to get a cached free pool. */
    pool = alloc->usable_arenas->freepools;
    if (pool != NULL) {
        /* Unlink from cached pools. */
        alloc->usable_arenas->freepools = pool->nextusedpool;
        --alloc->usable_arenas->nfreepools;
        if (alloc->usable_arenas->nfreepools == 0) {
            /* Wholly allocated:  remove. */
            assert(alloc->usable_arenas->freepools == NULL);
            assert(alloc->usable_arenas->nextarena == NULL ||
                   alloc->usable_arenas->nextarena->prevarena ==
                   alloc->usable_arenas);
            alloc->usable_arenas = alloc->usable_arenas->nextarena;
            if (alloc->usable_arenas != NULL) {
                alloc->usable_arenas->prevarena = NULL;
                assert(alloc->usable_arenas->address != 0);
            }
        }
        else {
            /* nfreepools > 0:  it must be that freepools
             * isn't NULL, or that we haven't yet carved
             * off all the arena's pools for the first
             * time.
             */
            assert(alloc->usable_arenas->freepools != NULL ||
                   alloc->usable_arenas->pool_address <=
                   (block*)alloc->usable_arenas->address +
                       GC_ARENA_SIZE - GC_POOL_SIZE);
        }

    init_pool:
        /* Frontlink to used pools. */
        next = alloc->usedpools[0]; /* == prev */
        pool->nextusedpool = next;
        pool->prevusedpool = next;
        next->nextusedpool = pool;
        next->prevusedpool = pool;

        /* Add to gen 0 */
        next = alloc->generations[0][0];
        pool->nextgenpool = next;
        pool->prevgenpool = next;
        next->nextgenpool = pool;
        next->prevgenpool = pool;

        pool->ref.count = 1;
        if (pool->szidx == 0) {
            /* Luckily, this pool last contained blocks
             * of the same size class, so its header
             * and free list are already initialized.
             */
            bp = pool->freeblock;
            assert(bp != NULL);
            pool->freeblock = *(block **)bp;
            goto success;
        }
        /*
         * Initialize the pool header, set up the free list to
         * contain just the second block, and return the first
         * block.
         */
        pool->szidx = 0;
        uint size = alloc->nbytes;
        bp = (block *)pool + POOL_OVERHEAD;
        pool->nextoffset = POOL_OVERHEAD + (size << 1);
        pool->maxnextoffset = GC_POOL_SIZE - size;
        pool->freeblock = bp + size;
        *(block **)(pool->freeblock) = NULL;
        memset(pool->gen_bitmaps, 0, sizeof(pool->gen_bitmaps));
        pool->is_young = 1;
        goto success;
    }

    /* Carve off a new pool. */
    assert(alloc->usable_arenas->nfreepools > 0);
    assert(alloc->usable_arenas->freepools == NULL);
    pool = (poolp)alloc->usable_arenas->pool_address;
    assert((block*)pool <= (block*)alloc->usable_arenas->address +
                             GC_ARENA_SIZE - GC_POOL_SIZE);
    pool->arenaindex = (uint)(alloc->usable_arenas - alloc->arenas);
    assert(&arenas[pool->arenaindex] == alloc->usable_arenas);
    pool->szidx = DUMMY_SIZE_IDX;
    alloc->usable_arenas->pool_address += GC_POOL_SIZE;
    --alloc->usable_arenas->nfreepools;

    if (alloc->usable_arenas->nfreepools == 0) {
        assert(alloc->usable_arenas->nextarena == NULL ||
               alloc->usable_arenas->nextarena->prevarena ==
               alloc->usable_arenas);
        /* Unlink the arena:  it is completely allocated. */
        alloc->usable_arenas = alloc->usable_arenas->nextarena;
        if (alloc->usable_arenas != NULL) {
            alloc->usable_arenas->prevarena = NULL;
            assert(alloc->usable_arenas->address != 0);
        }
    }

    goto init_pool;

success:
    assert(bp != NULL);

    set_bitmap(pool, bp);

    if (!pool->is_young) {
        // Unlink from current gen. Guaranteed to be part of a generation list
        // because the only pools that are not are the ones in freepools,
        // and if we picked a pool from there we already initialized it earlier
        // and set is_young=1
        next = pool->nextgenpool;
        prev = pool->prevgenpool;
        next->prevgenpool = prev;
        prev->nextgenpool = next;

        // link into gen 0
        next = alloc->generations[0][0];
        pool->nextgenpool = next;
        pool->prevgenpool = next;
        next->nextgenpool = pool;
        next->prevgenpool = pool;

        pool->is_young = 1;
    }

    return FROM_GC(bp);

failed:
    return NULL;
}


/* Free a memory block allocated by gcmalloc_alloc().
*/
void
gcmalloc_free(GCAllocator *alloc, void *p)
{
    p = AS_GC(p);

    poolp pool;
    block *lastfree;
    poolp next, prev;
    uint size;

    assert(p != NULL);

#ifdef WITH_VALGRIND
    if (UNLIKELY(running_on_valgrind > 0)) {
        return;
    }
#endif

    pool = POOL_ADDR(p);

    /* Link p to the start of the pool's freeblock list.  Since
     * the pool had at least the p block outstanding, the pool
     * wasn't empty (so it's already in a usedpools[] list, or
     * was full and is in no list -- it's not in the freeblocks
     * list in any case).
     */
    assert(pool->ref.count > 0);            /* else it was empty */
    *(block **)p = lastfree = pool->freeblock;
    pool->freeblock = (block *)p;
    if (!lastfree) {
        /* Pool was full, so doesn't currently live in any list:
         * link it to the front of the appropriate usedpools[] list.
         * This mimics LRU pool usage for new allocations and
         * targets optimal filling when several pools contain
         * blocks of the same size class.
         */
        --pool->ref.count;
        assert(pool->ref.count > 0);            /* else the pool is empty */
        size = pool->szidx;
        next = alloc->usedpools[size + size];
        prev = next->prevusedpool;

        /* insert pool before next:   prev <-> pool <-> next */
        pool->nextusedpool = next;
        pool->prevusedpool = prev;
        next->prevusedpool = pool;
        prev->nextusedpool = pool;
        goto success;
    }

    struct arena_object* ao;
    uint nf;  /* ao->nfreepools */

    /* freeblock wasn't NULL, so the pool wasn't full,
     * and the pool is in a usedpools[] list.
     */
    if (--pool->ref.count != 0) {
        /* pool isn't empty:  leave it in usedpools */
        goto success;
    }
    /* Pool is now empty:  unlink from usedpools, and
     * link to the front of freepools.  This ensures that
     * previously freed pools will be allocated later
     * (being not referenced, they are perhaps paged out).
     */
    next = pool->nextusedpool;
    prev = pool->prevusedpool;
    next->prevusedpool = prev;
    prev->nextusedpool = next;

    // Also unlink from the generation list
    next = pool->nextgenpool;
    prev = pool->prevgenpool;
    next->prevgenpool = prev;
    prev->nextgenpool = next;
    // This isn't strictly necessary but will make bugs show up faster:
    pool->nextgenpool = pool->prevgenpool = NULL;

    /* Link the pool to freepools.  This is a singly-linked
     * list, and pool->prevusedpool isn't used there.
     */
    ao = &alloc->arenas[pool->arenaindex];
    pool->nextusedpool = ao->freepools;
    ao->freepools = pool;
    nf = ao->nfreepools;
    /* If this is the rightmost arena with this number of free pools,
     * nfp2lasta[nf] needs to change.  Caution:  if nf is 0, there
     * are no arenas in usable_arenas with that value.
     */
    struct arena_object* lastnf = alloc->nfp2lasta[nf];
    assert((nf == 0 && lastnf == NULL) ||
           (nf > 0 &&
            lastnf != NULL &&
            lastnf->nfreepools == nf &&
            (lastnf->nextarena == NULL ||
             nf < lastnf->nextarena->nfreepools)));
    if (lastnf == ao) {  /* it is the rightmost */
        struct arena_object* p = ao->prevarena;
        alloc->nfp2lasta[nf] = (p != NULL && p->nfreepools == nf) ? p : NULL;
    }
    ao->nfreepools = ++nf;

    /* All the rest is arena management.  We just freed
     * a pool, and there are 4 cases for arena mgmt:
     * 1. If all the pools are free, return the arena to
     *    the system free().
     * 2. If this is the only free pool in the arena,
     *    add the arena back to the `usable_arenas` list.
     * 3. If the "next" arena has a smaller count of free
     *    pools, we have to "slide this arena right" to
     *    restore that usable_arenas is sorted in order of
     *    nfreepools.
     * 4. Else there's nothing more to do.
     */
    if (nf == ao->ntotalpools) {
        /* Case 1.  First unlink ao from usable_arenas.
         */
        assert(ao->prevarena == NULL ||
               ao->prevarena->address != 0);
        assert(ao ->nextarena == NULL ||
               ao->nextarena->address != 0);

        /* Fix the pointer in the prevarena, or the
         * usable_arenas pointer.
         */
        if (ao->prevarena == NULL) {
            alloc->usable_arenas = ao->nextarena;
            assert(alloc->usable_arenas == NULL ||
                   alloc->usable_arenas->address != 0);
        }
        else {
            assert(ao->prevarena->nextarena == ao);
            ao->prevarena->nextarena =
                ao->nextarena;
        }
        /* Fix the pointer in the nextarena. */
        if (ao->nextarena != NULL) {
            assert(ao->nextarena->prevarena == ao);
            ao->nextarena->prevarena =
                ao->prevarena;
        }
        /* Record that this arena_object slot is
         * available to be reused.
         */
        ao->nextarena = alloc->unused_arena_objects;
        alloc->unused_arena_objects = ao;

        /* Free the entire arena. */
#if PY_DEBUGGING_FEATURES
        _PyObject_Arena.free(_PyObject_Arena.ctx,
                             (void *)ao->address, GC_ARENA_SIZE);
#else
        _PyObject_ArenaMunmap(NULL,
                             (void *)ao->address, GC_ARENA_SIZE);
#endif
        ao->address = 0;                        /* mark unassociated */
        --alloc->narenas_currently_allocated;

        goto success;
    }

    if (nf == 1) {
        /* Case 2.  Put ao at the head of
         * usable_arenas.  Note that because
         * ao->nfreepools was 0 before, ao isn't
         * currently on the usable_arenas list.
         */
        ao->nextarena = alloc->usable_arenas;
        ao->prevarena = NULL;
        if (alloc->usable_arenas)
            alloc->usable_arenas->prevarena = ao;
        alloc->usable_arenas = ao;
        assert(alloc->usable_arenas->address != 0);
        if (alloc->nfp2lasta[1] == NULL) {
            alloc->nfp2lasta[1] = ao;
        }

        goto success;
    }

    /* If this arena is now out of order, we need to keep
     * the list sorted.  The list is kept sorted so that
     * the "most full" arenas are used first, which allows
     * the nearly empty arenas to be completely freed.  In
     * a few un-scientific tests, it seems like this
     * approach allowed a lot more memory to be freed.
     */
    /* If this is the only arena with nf, record that. */
    if (alloc->nfp2lasta[nf] == NULL) {
        alloc->nfp2lasta[nf] = ao;
    } /* else the rightmost with nf doesn't change */
    /* If this was the rightmost of the old size, it remains in place. */
    if (ao == lastnf) {
        /* Case 4.  Nothing to do. */
        goto success;
    }
    /* If ao were the only arena in the list, the last block would have
     * gotten us out.
     */
    assert(ao->nextarena != NULL);

    /* Case 3:  We have to move the arena towards the end of the list,
     * because it has more free pools than the arena to its right.  It needs
     * to move to follow lastnf.
     * First unlink ao from usable_arenas.
     */
    if (ao->prevarena != NULL) {
        /* ao isn't at the head of the list */
        assert(ao->prevarena->nextarena == ao);
        ao->prevarena->nextarena = ao->nextarena;
    }
    else {
        /* ao is at the head of the list */
        assert(alloc->usable_arenas == ao);
        alloc->usable_arenas = ao->nextarena;
    }
    ao->nextarena->prevarena = ao->prevarena;
    /* And insert after lastnf. */
    ao->prevarena = lastnf;
    ao->nextarena = lastnf->nextarena;
    if (ao->nextarena != NULL) {
        ao->nextarena->prevarena = ao;
    }
    lastnf->nextarena = ao;
    /* Verify that the swaps worked. */
    assert(ao->nextarena == NULL || nf <= ao->nextarena->nfreepools);
    assert(ao->prevarena == NULL || nf > ao->prevarena->nfreepools);
    assert(ao->nextarena == NULL || ao->nextarena->prevarena == ao);
    assert((alloc->usable_arenas == ao && ao->prevarena == NULL)
           || ao->prevarena->nextarena == ao);

    goto success;

success:
    return;
}
