#ifndef Py_INTERNAL_CODE_H
#define Py_INTERNAL_CODE_H
#ifdef __cplusplus
extern "C" {
#endif

enum _PyOpcache_LoadGlobal_Types {
    // Value came from the globals dictionary.
    // We guard on the ma_version of the globals dict, and can cache the exact object value
    LG_GLOBAL = 0,

    // Same as LG_GLOBAL but comes from the builtins dictionary, so we need to guard on
    // the versions of both the globals and builtins dictionaries.
    LG_BUILTIN = 1,

    // LG_GLOBAL and LG_BUILTIN only work if there are no modifications to the globals dictionary,
    // so LG_GLOBAL_OFFSET is for lookups that come from globals, and stores the offset into
    // the globals hashtable.
    LG_GLOBAL_OFFSET = 2,

    // TODO: we could add a dk_version-based cache as well, which would be faster than the offset cache,
    // as well as allow caching results from the builtins dict
};

typedef struct {
    union {
        struct {
            PyObject *ptr;  /* Cached pointer (borrowed reference) */
            uint64_t globals_ver;  /* ma_version of global dict */
        } global_cache;
        struct {
            PyObject *ptr;  /* Cached pointer (borrowed reference) */
            uint64_t globals_ver;  /* ma_version of global dict */
            uint64_t builtins_ver; /* ma_version of builtin dict */
        } builtin_cache;
        struct {
            Py_ssize_t dk_size;
            int64_t offset;
        } global_offset_cache;
    } u;

    char cache_type;
} _PyOpcache_LoadGlobal;

#if PYSTON_SPEEDUPS
struct PyDictKeysObject;

typedef struct {
    uint64_t type_ver;  /* tp_version_tag of type */
    PyObject *method;  /* Cached pointer (borrowed reference) */
    union {
        uint64_t dict_ver;  /* ma_version of obj dict */
        uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
    } u;
    char cache_type;  // 0=guard on dict version, 1=guard on split dict keys
} _PyOpcache_LoadMethod;


enum _PyOpcache_LoadAttr_Types {
    // we always guard on the type version - in addition:

    // caching an object from type or instance, guarded by instance dict version
    // (only used if the more powerful LA_CACHE_IDX_SPLIT_DICT or LA_CACHE_VALUE_CACHE_SPLIT_DICT is not possible)
    LA_CACHE_VALUE_CACHE_DICT = 0,

    // caching an index inside instance splitdict, guarded by the splitdict keys version (dict->ma_keys->dk_version_tag)
    // if available, otherwise by the keys pointer value.
    LA_CACHE_IDX_SPLIT_DICT = 1,

    // caching a data descriptor object, guarded by data descriptor types version
    LA_CACHE_DATA_DESCR = 2,

    // caching an object from the type, guarded by either the instance splitdict keys version (dict->ma_keys->dk_version_tag)
    // or the keys identity + keys nentries if the version is not available
    // (making sure the attribute is not getting overwritten in the instance dict)
    // Instances may have fewer attributes than there are keys, but that is ok because we just
    // need to prove that the instance does *not* have the relevant attribute.
    LA_CACHE_VALUE_CACHE_SPLIT_DICT = 3,

    // caching the offset to the instance dict entry inside the hash table.
    // Works for non split dicts but retrieval is slower than LA_CACHE_VALUE_CACHE_DICT
    // so only gets used if the lookups miss frequently.
    // Has the advantage that even with modifications to the dict the cache will mostly hit.
    // Guards on the hashtable size to ensure that the index points to a valid entry, but
    // doesn't need to guard on the number of entries in the hashtable since it checks the entry.
    LA_CACHE_OFFSET_CACHE = 4,

    // The same thing as LA_CACHE_OFFSET_CACHE but for split dicts.
    // Since the instance can have fewer attributes set than keys set, this cache can return NULL.
    LA_CACHE_OFFSET_CACHE_SPLIT = 5,

    // caching the offset to attribute slot inside a python object.
    // used for __slots__
    // LA_CACHE_DATA_DESCR works too but is slower because it needs extra guarding
    // and emits a call to the decriptor function
    LA_CACHE_SLOT_CACHE = 6,

    // This works similarly to LA_CACHE_VALUE_CACHE_DICT but is specifically
    // for immutable types, such as the builtins.
    // This lets us include the type in the cache entry instead of the tp_version_tag
    // (for two reasons: first we know that the type will stay alive so it's safe to
    // store a borrowed reference, and second we know the tp_version_tag won't change.)
    // This lets us constant-fold a number of the checks when we jit this load.
    LA_CACHE_BUILTIN = 7,

    // Used for polymorphic sites where we store an array of _PyOpcaches
    LA_CACHE_POLYMORPHIC = 8,
};
typedef struct {
    union {
        uint64_t type_ver;  /* tp_version_tag of type. Used for everything other than cache type 6 */
        PyTypeObject* type; /* type of the object. Only used for cache type 6 */
    };
    union {
        struct {
            PyObject *obj;
        } builtin_cache;
        struct {
            PyObject *obj;  /* Cached pointer (borrowed reference) */
            // guard on the exact instance dict version (dict_ver contains dict->ma_version)
            uint64_t dict_ver;
        } value_cache;
#ifdef NO_DKVERSION
        struct {
            // This struct notionally stores the following fields:
            // PyObject *obj;
            // void* keys_obj;
            // Py_ssize_t dk_nentries;
            // But since that would make this the largest struct in the union,
            // we try a bit harder to save space. Both obj and keys_obj are
            // 16-bit aligned, so we steal the bottom 4 bits of each to store dk_nentries,
            // and punt on caching this cache type if there are >=256 attributes
            uintptr_t obj_and_nentries;
            uintptr_t keysobj_and_nentries;
        } value_cache_split;
#else
        struct {
            PyObject *obj;  /* Cached pointer (borrowed reference) */
            uint64_t dk_version;
        } value_cache_split;
#endif
        struct {
#ifdef NO_DKVERSION
            void* keys_obj;
#else
            uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
#endif
            Py_ssize_t splitdict_index;  /* index into dict value array */
        } split_dict_cache;
        struct {
            PyObject *descr;  /* Cached pointer (borrowed reference) */
            uint64_t descr_type_ver;  /* tp_version_tag of the descriptor type */
        } descr_cache;
        struct {
            Py_ssize_t dk_size; /* dk_size of the dict */
            int64_t offset; /* offset in bytes from ma_keys->dk_indices to the item in the hash table */
        } offset_cache;
        struct {
            Py_ssize_t dk_size; /* dk_size of the dict */
            int64_t ix; /* index in the table */
        } offset_cache_split;
        struct {
            int64_t offset; /* offset in bytes from the start of the PyObject to the slot */
        } slot_cache;
        struct {
            struct _PyOpcache* caches; // pointer to array of _PyOpcaches with num_entries
            char num_entries;
            char num_used;
        } poly_cache;
    } u;
    short type_tp_dictoffset;  /* tp_dictoffset of type */
    char cache_type;
    char meth_found; // used by LOAD_METHOD: can we do the method descriptor optimization or not
    char guard_tp_descr_get; // do we have to guard on Py_TYPE(u.value_cache.obj)->tp_descr_get == NULL
    uint8_t type_hash; // used as heuristic to decide if cache entry should be rewritten or switched to polymorphic
} _PyOpcache_LoadAttr;

enum _PyOpcache_StoreAttr_Types {
    // we always guard on the type version - in addition:

    // caching an index inside instance splitdict, guarded by the splitdict keys version (dict->ma_keys->dk_version_tag)
    // if available, otherwise by the keys pointer value.
    // When we guard on the pointer value it's possible that additional keys have been added since we populated
    // the cache, but this is ok because it won't change the index of the attribute in question.
    // (If enough keys are added that the keys are resized, then the keys pointer will change.)
    SA_CACHE_IDX_SPLIT_DICT = 0,
    SA_CACHE_IDX_SPLIT_DICT_INIT = 1, // same as the first but means we hit the dict not initialized path

    // caching the offset to attribute slot inside a python object.
    // used for __slots__
    SA_CACHE_SLOT_CACHE = 2,
};

typedef struct {
    uint64_t type_ver;  /* tp_version_tag of type */
    union {
        struct {
#ifdef NO_DKVERSION
            void* keys_obj;
#else
            uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
#endif
            Py_ssize_t splitdict_index;  /* index into dict value array */
        } split_dict_cache;
        struct {
            int64_t offset; /* offset in bytes from the start of the PyObject to the slot */
        } slot_cache;
    } u;
    short type_tp_dictoffset;  /* tp_dictoffset of type */
    char cache_type;
} _PyOpcache_StoreAttr;

typedef struct {
    PyTypeObject* type;  /* borrowed type */
} _PyOpcache_Type;

typedef struct {
    PyTypeObject* type;  /* borrowed type */
    unsigned char refcnt1_left; /* how many times had the left operand a refcnt of 1 */
    unsigned char refcnt2_left; /* how many times had the left operand a refcnt of 2 */
    unsigned char refcnt1_right;/* how many times had the right operand a refcnt of 1 */
} _PyOpcache_TypeRefcnt;

#endif

struct _PyOpcache {
    union {
        _PyOpcache_LoadGlobal lg;
#if PYSTON_SPEEDUPS
        _PyOpcache_LoadMethod lm;
        _PyOpcache_LoadAttr la;
        _PyOpcache_StoreAttr sa;
        _PyOpcache_Type t;
        _PyOpcache_TypeRefcnt t_refcnt;
#endif
    } u;
    char optimized;
#if PYSTON_SPEEDUPS
    char num_failed;
#endif
};

#if PYSTON_SPEEDUPS
typedef struct _PyOpcache _PyOpcache;
#endif

/* Private API */
int _PyCode_InitOpcache(PyCodeObject *co);

// Opcache handling for pyston-lite:
// The default opcache is located directly in the PyCodeObject, and in pyston-full we use that.
// In pyston-lite we need to store this data in co_extra to not conflict with CPython's usage
// of the opcache fields.
//
// To enable writing code that handles both cases, there's now a new `OpCache` type that has
// the opcache fields on it, that is used something like
//      PyCodeObject* co;
//      OpCache* opcache = _PyCode_GetOpcache(co);
//      opcache->oc_opcache_flag++;
//
// For pyston-full, OpCache is just a synonym for PyCodeObject, and looking up attributes on the OpCache
// object just resolves to the code object. I believe this should end up generating the same code
// for the interpreter since the compiler should know they are synonyms.
// For pyston-lite, we store a real OpCache struct in the co_extra section of the code object

#ifdef PYSTON_LITE

// Defensiveness: it's a bug to use the CPython-managed fields, so lets make them compile errors:
#define co_opcache_map DONTUSE
// There are local variables called this unfortunately:
//#define co_opcache DONTUSE
#define co_opcache_flag DONTUSE
#define co_opcache_size DONTUSE

typedef struct {
    unsigned char *oc_opcache_map;
    _PyOpcache *oc_opcache;
    long oc_opcache_flag;
    unsigned char oc_opcache_size;
} OpCache;

OpCache* _PyCode_GetOpcache(PyCodeObject *co);
int _PyCode_InitOpcache_Pyston(PyCodeObject *co, OpCache *opcache);

#else

#define OpCache PyCodeObject
#define _PyCode_GetOpcache(co) (co)

// Hacky, but simply map these attribute names to the normal ones
#define oc_opcache_map co_opcache_map
#define oc_opcache co_opcache
#define oc_opcache_flag co_opcache_flag
#define oc_opcache_size co_opcache_size
#endif


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CODE_H */
