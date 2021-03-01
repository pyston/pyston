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

#ifndef PYSTON_CLEANUP
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

    // caching a data descriptor object, guarded by data descripor types version
    LA_CACHE_DATA_DESCR = 2,

    // caching an object from the type, guarded by instance splitdict keys version (dict->ma_keys->dk_version_tag)
    // (making sure the attribute is not getting overwritten in the instance dict)
    LA_CACHE_VALUE_CACHE_SPLIT_DICT = 3,

    // caching the offset to the instance dict entry inside the hash table.
    // Works for non split dicts but retrieval is slower than LA_CACHE_VALUE_CACHE_DICT
    // so only gets used if the lookups miss frequently.
    // Has the advantage that even with modifications to the dict the cache will mostly hit.
    LA_CACHE_OFFSET_CACHE = 4,
};
typedef struct {
    uint64_t type_ver;  /* tp_version_tag of type */
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
    } u;
    char cache_type;
    char meth_found; // used by LOAD_METHOD: can we do the method descriptor optimization or not
    char guard_tp_descr_get; // do we have to guard on Py_TYPE(u.value_cache.obj)->tp_descr_get == NULL
} _PyOpcache_LoadAttr;

typedef struct {
    uint64_t type_ver;  /* tp_version_tag of type */
    uint64_t splitdict_keys_version;  /* dk_version_tag of dict */
    Py_ssize_t splitdict_index;  /* index into dict value array */
} _PyOpcache_StoreAttr;

_Static_assert(sizeof(_PyOpcache_LoadMethod) <= 32,  "_data[32] needs to be updated");
_Static_assert(sizeof(_PyOpcache_LoadAttr) <= 32,  "_data[32] needs to be updated");
_Static_assert(sizeof(_PyOpcache_StoreAttr) <= 32,  "_data[32] needs to be updated");
#endif
#endif

struct _PyOpcache {
    union {
        _PyOpcache_LoadGlobal lg;
#if PYSTON_SPEEDUPS
#ifndef PYSTON_CLEANUP
        _PyOpcache_LoadMethod lm;
        _PyOpcache_LoadAttr la;
        _PyOpcache_StoreAttr sa;
#else
        char _data[32];
#endif
#endif
    } u;
    char optimized;
#if PYSTON_SPEEDUPS
#ifndef PYSTON_CLEANUP
    char num_failed;
#else
    char _data2;
#endif
#endif
};

/* Private API */
int _PyCode_InitOpcache(PyCodeObject *co);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CODE_H */
