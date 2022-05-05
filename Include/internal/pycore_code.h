#ifndef Py_INTERNAL_CODE_H
#define Py_INTERNAL_CODE_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    PyObject *ptr;  /* Cached pointer (borrowed reference) */
    uint64_t globals_ver;  /* ma_version of global dict */
    uint64_t builtins_ver; /* ma_version of builtin dict */
} _PyOpcache_LoadGlobal;

// This is a special value for the builtins_ver field
// that specifies that the LOAD_GLOBAL hit came from the globals
// and thus the builtins version doesn't matter.
#define LOADGLOBAL_WAS_GLOBAL UINT64_MAX

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
    LA_CACHE_IDX_SPLIT_DICT = 1,

    // caching a data descriptor object, guarded by data descriptor types version
    LA_CACHE_DATA_DESCR = 2,

    // caching an object from the type, guarded by instance splitdict keys version (dict->ma_keys->dk_version_tag)
    // (making sure the attribute is not getting overwritten in the instance dict)
    LA_CACHE_VALUE_CACHE_SPLIT_DICT = 3,

    // caching the offset to the instance dict entry inside the hash table.
    // Works for non split dicts but retrieval is slower than LA_CACHE_VALUE_CACHE_DICT
    // so only gets used if the lookups miss frequently.
    // Has the advantage that even with modifications to the dict the cache will mostly hit.
    LA_CACHE_OFFSET_CACHE = 4,

    // caching the offset to attribute slot inside a python object.
    // used for __slots__
    // LA_CACHE_DATA_DESCR works too but is slower because it needs extra guarding
    // and emits a call to the decriptor function
    LA_CACHE_SLOT_CACHE = 5,

    // This works similarly to LA_CACHE_VALUE_CACHE_DICT but is specifically
    // for immutable types, such as the builtins.
    // This lets us include the type in the cache entry instead of the tp_version_tag
    // (for two reasons: first we know that the type will stay alive so it's safe to
    // store a borrowed reference, and second we know the tp_version_tag won't change.)
    // This lets us constant-fold a number of the checks when we jit this load.
    LA_CACHE_BUILTIN = 6,
};
typedef struct {
    union {
        uint64_t type_ver;  /* tp_version_tag of type. Used for everything other than cache type 6 */
        PyTypeObject* type; /* type of the object. Only used for cache type 6 */
    };
    union {
        struct {
            PyObject *obj;  /* Cached pointer (borrowed reference) */
            /* cache_type=0 guard on the exact instance dict version (dict_ver contains dict->ma_version)
               cache_type=3 guard on instance split dict keys not changing (dict_ver contains dict->ma_keys->dk_version_tag)
                (used when we guard that a attribute is coming from the type and is not inside the instance dict) */
            uint64_t dict_ver;
        } value_cache;
        struct {
            uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
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
            int64_t offset; /* offset in bytes from the start of the PyObject to the slot */
        } slot_cache;
    } u;
    short type_tp_dictoffset;  /* tp_dictoffset of type */
    char cache_type;
    char meth_found; // used by LOAD_METHOD: can we do the method descriptor optimization or not
    char guard_tp_descr_get; // do we have to guard on Py_TYPE(u.value_cache.obj)->tp_descr_get == NULL
} _PyOpcache_LoadAttr;

enum _PyOpcache_StoreAttr_Types {
    // we always guard on the type version - in addition:

    // caching an index inside instance splitdict, guarded by the splitdict keys version (dict->ma_keys->dk_version_tag)
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
            uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
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

_Static_assert(sizeof(_PyOpcache_LoadMethod) <= 32,  "_data[32] needs to be updated");
_Static_assert(sizeof(_PyOpcache_LoadAttr) <= 32,  "_data[32] needs to be updated");
_Static_assert(sizeof(_PyOpcache_StoreAttr) <= 32,  "_data[32] needs to be updated");
_Static_assert(sizeof(_PyOpcache_Type) <= 32,  "_data[32] needs to be updated");
#endif

struct _PyOpcache {
    union {
        _PyOpcache_LoadGlobal lg;
#if PYSTON_SPEEDUPS
        _PyOpcache_LoadMethod lm;
        _PyOpcache_LoadAttr la;
        _PyOpcache_StoreAttr sa;
        _PyOpcache_Type t;
#endif
    } u;
    char optimized;
#if PYSTON_SPEEDUPS
    char num_failed;
#endif
};

/* Private API */
int _PyCode_InitOpcache(PyCodeObject *co);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CODE_H */
