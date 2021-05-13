#!/usr/bin/python3

import copy
import ctypes
import itertools
import multiprocessing.pool
import os
import sys

cmps = ["PyCmp_LT", "PyCmp_LE", "PyCmp_EQ", "PyCmp_NE",
        "PyCmp_GT", "PyCmp_GE", "PyCmp_IN", "PyCmp_NOT_IN",
        ]

# this functions return an int and we need to wrap it into a python object
funcs_need_res_wrap = [
    "PyObject_DelItem",
    "PyObject_SetItem",
]

VERBOSITY = 0


class AttributeGuard(object):
    def __init__(self, accessor, guard_val):
        " accessor is expected to be something like '->ob_type' "
        self.accessor = accessor
        self.guard_val = guard_val

    def getGuard(self, variable_name):
        return f'{variable_name}{self.accessor} == {self.guard_val}'

class TypeGuard(AttributeGuard):
    def __init__(self, guard_val):
        super(TypeGuard, self).__init__("->ob_type", guard_val)

class IdentityGuard(AttributeGuard):
    def __init__(self, guard_val):
        super(IdentityGuard, self).__init__("", guard_val)

class NullGuard(object):
    def getGuard(self, variable_name):
        return "1"
Unspecialized = NullGuard()
NullGuard.__init__ = None

class ObjectClass(object):
    def __init__(self, name, guard, examples):
        # TODO: do some name-clash detection?
        # We currently have a name clash though between Float as an argument type
        # and Float as a callable value.  They're never used for the same function
        # Maybe it's ok to rely on the c compiler to catch name duplication later
        self.name = name
        self.guard = guard
        self.examples = examples

    def __repr__(self):
        return "<ObjectClass %s>" % self.name

class Signature(object):
    def __init__(self, argument_classes):
        self.argument_classes = argument_classes
        self.nargs = len(argument_classes)
        self.name = "".join([cls.name for cls in argument_classes]) + str(self.nargs)

    def getGuard(self, argument_names):
        names = list(argument_names)
        for i in range(len(names), len(self.argument_classes)):
            assert self.argument_classes[i].guard is Unspecialized
            names.append(None)

        assert len(names) == len(self.argument_classes), (len(names), len(self.argument_classes), argument_names)
        guards = [cls.guard.getGuard(name) for (cls, name) in zip(self.argument_classes, names)]
        guards = [g for g in guards if g != "1"]

        return " && ".join(guards)

    def getExamples(self):
        for x in itertools.product(*[cls.examples for cls in self.argument_classes]):
            yield copy.deepcopy(x)

    def __repr__(self):
        return "<Signature %s>" % self.name

class CompoundSignature(object):
    def __init__(self, signatures):
        self.signatures = signatures

        names = [s.name for s in signatures]
        assert len(set(names)) == 1
        self.name = names[0]

    def getGuard(self, argument_names):
        guards = [s.getGuard(argument_names) for s in self.signatures]
        assert len(set(guards)) == 1, set(guards)
        return guards[0]

    def getExamples(self):
        for s in self.signatures:
            yield from s.getExamples()

    @property
    def nargs(self):
        nargs = [s.nargs for s in self.signatures]
        assert len(set(nargs)) == 1
        return nargs[0]

class FunctionCases(object):
    def __init__(self, c_func_name, signatures):
        self.unspecialized_name = c_func_name
        self.signatures = signatures

    def getSignatures(self):
        for s in self.signatures:
            yield s, f'{self.unspecialized_name}{s.name}'

    @property
    def nargs(self):
        nargs = [s.nargs for s in self.signatures]
        assert len(set(nargs)) == 1, nargs
        return nargs[0]

    def __repr__(self):
        return f'<FunctionCases of {self.unspecialized_name} with {len(self.signatures)} cases>'

class Handler(object):
    def __init__(self, case):
        self.case = case

class NormalHandler(Handler):
    """
    Handler object for handling "normal" functions that take their arguments normally
    Such as PyNumber_Add
    """

    def __init__(self, case):
        super(NormalHandler, self).__init__(case)
        self.need_res_wrap = case.unspecialized_name in funcs_need_res_wrap
        self.nargs = case.nargs

    def writePretraceFunctions(self, f):
        unspecialized_name = self.case.unspecialized_name
        for signature, name in self.case.getSignatures():
            arg_names = [f"o{i}" for i in range(signature.nargs)]
            parameters = [f"PyObject *{name}" for name in arg_names]
            pass_args = ", ".join(arg_names)

            print(f"{self._get_ret_type()} {name}({', '.join(parameters)})", "{", file=f)
            print(f"  if (unlikely(!({signature.getGuard(arg_names)})))", "{", file=f)
            print(f"    SET_JIT_AOT_FUNC({unspecialized_name});", file=f)
            print(f"    PyObject* ret = {unspecialized_name}({pass_args});", file=f)
            # this makes sure the compiler is not merging the two calls into a single one
            print(f"    __builtin_assume(ret != (PyObject*)0x1);", file=f)
            print(f"    return ret;",  file=f)
            print("  }", file=f)

            for arg in arg_names:
                print(f"  __builtin_assume({arg} != NULL);", file=f)

            print(f"  return {unspecialized_name}({pass_args});", file=f)
            print("}", file=f)

    def createJitTarget(self, target_func, ntraces):
        return createJitTarget(target_func, self.case.nargs, ntraces)

    def call(self, target_func, args):
        assert len(args) == self.case.nargs

        target_func.argtypes = [ctypes.py_object] * len(args)
        if not self.need_res_wrap:
            target_func.restype = ctypes.py_object

        return target_func(*map(ctypes.py_object, args))

    def _o_args_names(self):
        return [f"o{i}" for i in range(self.nargs)]

    def _args_names(self):
        return [name for (_, name) in self._args()]

    def _args(self):
        oargs = [('PyObject *', o) for o in self._o_args_names()]
        return oargs

    def _get_func_sig(self, name):
        param = ", ".join([f"{type} {o}" for (type, o) in self._args()])
        return f"{self._get_ret_type()} {name}({param})"

    def _get_ret_type(self):
        if self.need_res_wrap:
            return "int"
        return "PyObject*"

    def write_profile_func(self, traced, header_f, profile_f):
        for signature, name in traced:
            print(f"{self._get_func_sig(name)};", file=profile_f)
            print(f"{self._get_func_sig(name)};", file=header_f)

        unspec_name = self.case.unspecialized_name
        profile_name = unspec_name + 'Profile'
        profile_func_sig = self._get_func_sig(profile_name)
        print(f"{profile_func_sig};", file=header_f)
        print(f"{profile_func_sig}", "{", file=profile_f)
        func_call_args = ', '.join(self._args_names())

        for spec, name in traced:
            checks = spec.getGuard([f"o{i}" for i in range(self.nargs)])
            print(f"  if ({checks})", "{", file=profile_f)
            print(f"    SET_JIT_AOT_FUNC({name});", file=profile_f)
            print(f"    return {name}({func_call_args});", file=profile_f)
            print("  }", file=profile_f)


        format_str = '%s ' * self.nargs
        dbg_args = ", ".join(
            f'{o}->ob_type->tp_name' for o in self._o_args_names())
        print(
            rf'  DBG("Missing {unspec_name} {format_str}\n", {dbg_args});', file=profile_f)
        print(f"  SET_JIT_AOT_FUNC({unspec_name});", file=profile_f)
        print(f"  return {unspec_name}({', '.join(self._args_names())});", file=profile_f)
        print("}", file=profile_f)

    def trace(self, jit_target, args):
        return runJitTarget(jit_target, *args, wrap_result=self.need_res_wrap)

class CallableHandler(Handler):
    """
    Handler object for callables, which have a special calling convention
    """

    def __init__(self, case):
        super(CallableHandler, self).__init__(case)

        self._jit_targets = {}

    def writePretraceFunctions(self, f):
        for signature, name in self.case.getSignatures():
            arg_names = ["f"]
            nargs = signature.nargs
            print(
                f"PyObject* {name}(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg)", "{", file=f)
            print(f"  if (unlikely(oparg != {nargs-1}))", "{" , file=f)
            print(f"    SET_JIT_AOT_FUNC({self.case.unspecialized_name});", file=f)
            print(f"    PyObject* ret = {self.case.unspecialized_name}(tstate, pp_stack, oparg);", file=f)
            # this makes sure the compiler is not merging the calls into a single one
            print(f"    __builtin_assume(ret != (PyObject*)0x1);", file=f)
            print(f"    return ret;", file=f)
            print("  }", file=f)
            print(f"  PyObject* f = *((*pp_stack) - oparg - 1);", file=f)
            print(f"  if (unlikely(!({signature.getGuard(arg_names)})))", "{", file=f)
            print(f"    SET_JIT_AOT_FUNC({self.case.unspecialized_name});", file=f)
            print(f"    PyObject* ret = {self.case.unspecialized_name}(tstate, pp_stack, oparg);", file=f)
            # this makes sure the compiler is not merging the calls into a single one
            print(f"    __builtin_assume(ret != (PyObject*)0x1);", file=f)
            print(f"    return ret;", file=f)
            print("  }", file=f)
            print(f"  return {self.case.unspecialized_name}(tstate, pp_stack, oparg);", file=f)
            print("}", file=f)

    def createJitTarget(self, target_func, ntraces):
        return createJitTarget(target_func, 3, ntraces)

    def call(self, target_func, args):
        # TODO: this shouldn't trace, but we don't yet have a call_helper equivalent that doesn't trace
        # get address of the actual function written in C not the python object
        key = ctypes.cast(target_func, ctypes.c_void_p).value
        if key not in self._jit_targets:
            self._jit_targets[key] = self.createJitTarget(target_func, 0xDEAD)

        return call_helper(self._jit_targets[key], *args)

    def write_profile_func(self, traced, header_f, profile_f):
        func = self.case.unspecialized_name
        profile_func = self.case.unspecialized_name + 'Profile'
        print(f"PyObject* {func}(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg);", file=header_f)

        for signature, name in self.case.getSignatures():
            print(
                f"PyObject* {name}(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg);", file=header_f)
            print(
                f"PyObject* {name}(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg);", file=profile_f)

        print(
            f"PyObject* {func}Profile(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg);", file=header_f)
        print(
            f"PyObject* {func}Profile(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg)", "{", file=profile_f)
        print("PyObject* f = *((*pp_stack) - oparg - 1);", file=profile_f)

        for signature, name in traced:
            guard = signature.getGuard(["f"])
            nargs = signature.nargs
            print(f"  if (oparg == {nargs-1} && {guard})", "{", file=profile_f)
            print(f"    SET_JIT_AOT_FUNC({name});", file=profile_f)
            print(f"    return {name}(tstate, pp_stack, oparg);", file=profile_f)
            print("}", file=profile_f)
        print(
            rf'  DBG("Missing {func} %s\n", f->ob_type == &PyType_Type ? ((PyTypeObject*)f)->tp_name : f->ob_type->tp_name);', file=profile_f)
        print(f"  SET_JIT_AOT_FUNC({func});", file=profile_f)
        print(f"  return {func}(tstate, pp_stack, oparg);", file=profile_f)
        print("}", file=profile_f)


    def trace(self, jit_target, args):
        return call_helper(jit_target, *args)

# These lambdas have to be defined at the global level.
# If they are defined inside a function (with the rest of the data), they
# are marked as "nested" functions which are called differently (and more slowly)
# than top-level functions.
def foo0():
    return 42
def foo1(x):
    return 42
def foo2(x, y):
    return 42
def foo3(x, y, z):
    return 42
def foo4(x, y, z, w):
    return 42
def foo5(x, y, z, w, v):
    return 42
def foo6(x, y, z, w, v, u):
    return 42
def foo7(x, y, z, w, v, u, a):
    return 42
def foo8(x, y, z, w, v, u, a, b):
    return 42
_function_cases = []
for i in range(0, 9):
    _function_cases.append((globals()[f"foo{i}"], tuple(range(i))))

for i in range(0, 10):
    exec(f"""
class ExampleClass{i}:
    def __init__(self, {', '.join("arg%d" % x for x in range(i))}):
        pass
    """, globals())

def loadCases():
    funcs1 = ["PyNumber_Positive",
              "PyNumber_Negative",
              "PyNumber_Invert",

              "PyObject_GetIter",
              ]

    funcs2 = ["PyNumber_Multiply",
              "PyNumber_MatrixMultiply",
              "PyNumber_TrueDivide",
              "PyNumber_FloorDivide",
              "PyNumber_Remainder",
              "PyNumber_Add",
              "PyNumber_Subtract",
              "PyNumber_Lshift",
              "PyNumber_Rshift",
              "PyNumber_And",
              "PyNumber_Xor",
              "PyNumber_Or",
              "PyNumber_InPlaceMultiply",
              "PyNumber_InPlaceMatrixMultiply",
              "PyNumber_InPlaceTrueDivide",
              "PyNumber_InPlaceFloorDivide",
              "PyNumber_InPlaceRemainder",
              "PyNumber_InPlaceAdd",
              "PyNumber_InPlaceSubtract",
              "PyNumber_InPlaceLshift",
              "PyNumber_InPlaceRshift",
              "PyNumber_InPlaceAnd",
              "PyNumber_InPlaceXor",
              "PyNumber_InPlaceOr",

              # they are specially made by us because they normally take 3 args and last is always None
              "PyNumber_PowerNone",
              "PyNumber_InPlacePowerNone",

              ] + ["cmp_outcome" + cmp for cmp in cmps if "IN" not in cmp]


    funcs3_2specialized = [
                           ]

    f = 4.4584
    types = {"Long": (0, 1, 4200, -42222),
             "Float": (f,),
             "Unicode": ('string', '', ' alaslas' * 20, 'a'),
             "List": ([1, 2, 3], list(range(100)),),
             "Tuple": ((1, 2, 3), (1., 2., 3.,) * 15,),
             "Range": (range(5), range(100),),
             "Dict": ({"a": 1, "b": 2, 1: 42, f: 0, None: None, 0: 0}, dict(),),
             "Set": ({"a", "b"}, set(),),
             "Bool": (True, False),
             "Slice": (slice(1, 2), slice(1), slice(1, 2, 2)),
             "None": (None,),
             }
    def getCTypeName(s):
        if s == "None":
            return "_PyNone_Type"
        if s == "Object":
            return "PyBaseObject_Type";
        return f"Py{s}_Type"
    type_classes = {k:ObjectClass(k, TypeGuard("&" + getCTypeName(k)), v) for (k, v) in types.items()}

    # look at types.py
    callables = {"CFunction": [(globals, ()), # zero args
                               (len, (([1, 2, 3], )), ((1, 2), ), ("test string", )), # one arg
                               (isinstance, (5, int), (42, str)), # two arg
                               (getattr, (5, "does not exist", 42))],  # three arg
             "MethodDescr": [(str.join, (" ", ("a", "b", "c"))), (str.upper, ("hello world",))],
             # "Method": [("aaa".lower, ())], #[].append),
             "Function": _function_cases,

             # Types
             # "Long": [(int, (1,), (1.5,), ("42", ))],
             "Float": [(float, (1,), (1.5,), ("42.5", ))],
             "Unicode": [(str, (1,), (1.5,), ("42.5", ))],
             "Range": [(range, (5,), (100,))],
             "Bool": [(bool, (1,), (False,))],
             "Type": [(type, (1,))],
             "Object": [(object, ())],
             # "Tuple": [(tuple, ([1,2,3], ), ((1,2,3), ))]

             # TODO: e.g.:
             # super, list, tuple, set, staticmethod, classmethod, property
             # what can we do for python classes?

             }

    cases = []

    call_signatures = []
    for name, l in callables.items():
        signatures = {}
        for example in l:
            func = example[0]
            for args in example[1:]:
                classes = []
                if isinstance(func, type):
                    classes.append(ObjectClass(name, IdentityGuard(f"(PyObject*)&{getCTypeName(name)}"), [func]))
                else:
                    classes.append(ObjectClass(name, TypeGuard(f"&{getCTypeName(name)}"), [func]))

                for arg in args:
                    classes.append(ObjectClass("", Unspecialized, [arg]))

                signatures.setdefault(len(args), []).append(Signature(classes))

        for nargs, sigs in signatures.items():
            if len(sigs) == 1:
                call_signatures.append(sigs[0])
            else:
                call_signatures.append(CompoundSignature(sigs))

    for nargs in range(0, 10):
        ctor_class = ObjectClass("Constructor", TypeGuard(f"&PyType_Type"), [globals()["ExampleClass%d" % nargs]])
        call_signatures.append(Signature([ctor_class] + [ObjectClass("", Unspecialized, [None])] * nargs))

    cases.append(CallableHandler(FunctionCases("call_function_ceval_no_kw", call_signatures)))
    cases.append(CallableHandler(FunctionCases("call_method_ceval_no_kw", call_signatures)))

    def makeSignatures(*args_classes):
        return [Signature(classes) for classes in itertools.product(*args_classes)]
    placeholder_class = ObjectClass("", Unspecialized, [(42, "test"), 0])

    for funcs, nargs, nspecialized in [(funcs1, 1, 1), (funcs2, 2, 2), (funcs3_2specialized, 3, 2)]:
        classes = [type_classes.values()] * nspecialized + [[placeholder_class]] * (nargs - nspecialized)

        signatures = makeSignatures(*classes)

        for func in funcs:
            cases.append(NormalHandler(FunctionCases(func, signatures)))

    # We currently don't do any specialization on the key argument for GetItem / SetItem / DelItem / IN / NOT_IN
    # so collapse all those traces into a single one

    getitem_signatures = makeSignatures(type_classes.values(), [placeholder_class])
    # getitem_signatures = [s for s in getitem_signatures if s.argument_classes[0].name != "Range"]
    cases.append(NormalHandler(FunctionCases("PyObject_GetItem", getitem_signatures)))

    setitem_signatures = makeSignatures([type_classes["List"]], [type_classes["Long"], type_classes["Slice"]], [placeholder_class])
    setitem_signatures += makeSignatures([type_classes["Dict"]], [placeholder_class], [placeholder_class])
    cases.append(NormalHandler(FunctionCases("PyObject_SetItem", setitem_signatures)))

    delitem_signatures = makeSignatures([type_classes["List"]], [type_classes["Long"], type_classes["Slice"]])
    delitem_signatures += makeSignatures([type_classes["Dict"]], [placeholder_class])
    cases.append(NormalHandler(FunctionCases("PyObject_DelItem", delitem_signatures)))

    in_signatures = makeSignatures([placeholder_class], type_classes.values())
    cases.append(NormalHandler(FunctionCases("cmp_outcomePyCmp_IN", in_signatures)))
    cases.append(NormalHandler(FunctionCases("cmp_outcomePyCmp_NOT_IN", in_signatures)))

    return cases

cases = loadCases()

def loadLibs():
    # TODO we should probably export a python module instead
    # say that its a pylib because otherwise we will release the gil and we can't access python object
    import ctypes
    nitrous_so = ctypes.PyDLL("libinterp.so")
    pystol_so = ctypes.PyDLL("libpystol.so")

    global initializeJIT
    initializeJIT = nitrous_so.initializeJIT
    initializeJIT.argtypes = [ctypes.c_long, ctypes.c_bool]

    global loadBitcode
    loadBitcode = nitrous_so.loadBitcode
    loadBitcode.argtypes = [ctypes.c_char_p]

    global createJitTarget
    createJitTarget = nitrous_so.createJitTarget
    createJitTarget.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_long]
    createJitTarget.restype = ctypes.c_void_p

    global aot_pre_trace_so
    aot_pre_trace_so = ctypes.PyDLL("./aot_pre_trace.so")

    def makeCType(obj):
        if isinstance(obj, ctypes._SimpleCData):
            return obj
        return ctypes.py_object(obj)

    runJitTargets = {}
    for wrap in ("", "_wrap_result"):
        for arg_num in range(5):
            c = aot_pre_trace_so[f"runJitTarget{arg_num}{wrap}_helper"]
            c.restype = ctypes.py_object
            c.argtypes = [ctypes.c_void_p] + [ctypes.py_object] * arg_num
            runJitTargets[(arg_num, bool(len(wrap)))] = c

    # wrap_result: if true wraps c int return value as PyLong
    def runJitTarget_wrapper(target, *args, wrap_result=False):
        py_args = map(makeCType, args)
        return runJitTargets[(len(args), wrap_result)](target, *py_args)
    global runJitTarget
    runJitTarget = runJitTarget_wrapper

    global pystolGlobalPythonSetup
    pystolGlobalPythonSetup = pystol_so.pystolGlobalPythonSetup

    call_helpers = []
    for arg_num in range(10):
        c = aot_pre_trace_so[f"call_helper{arg_num}"]
        c.argtypes = [ctypes.c_void_p, ctypes.py_object] + \
            ([ctypes.py_object] * arg_num)
        c.restype = ctypes.py_object
        call_helpers.append(c)

    def call_helper_wrapper(target, func, *args):
        return call_helpers[len(args)](target, ctypes.py_object(func), *map(makeCType, args))
    global call_helper
    call_helper = call_helper_wrapper


def specialize_func(handler, header_f, profile_f, async_tracing, only=None):
    unspecialized_name = handler.case.unspecialized_name
    print(f"Generating special versions of {unspecialized_name}")

    num_skipped = 0

    traced = []

    for signature, name in handler.case.getSignatures():
        if only and name != only:
            continue

        target_addr = aot_pre_trace_so[name]
        # we prune specialization which generate an error or inputs which generate one
        train_success = []

        for args in signature.getExamples():
            try:
                # If we set "only", skip this initial call so that we get
                # right to tracing
                if only is None or not isinstance(handler, CallableHandler):
                    handler.call(target_addr, copy.deepcopy(args))
                train_success.append(args)
            except:
                pass

        should_jit = len(train_success) > 0
        if should_jit:
            # create new jit target because we want to make sure we did not train on any error
            # TODO: could skip this if no errors happened
            target = handler.createJitTarget(target_addr, len(train_success))
            async_tracing.append((name, handler, target, copy.deepcopy(train_success)))
            traced.append((signature, name))
        else:
            num_skipped += 1
            #print(f"  {spec_name} errors. skipping...")

    print(f'  going to generate {len(traced)} special versions')
    if only is None:
        handler.write_profile_func(traced, header_f, profile_f)
    return (len(traced), num_skipped)


def do_trace(work):
    (name, handler, target, train_success) = work
    if VERBOSITY >= 1:
        print("tracing", name)

    for args in copy.deepcopy(train_success):
        handler.trace(target, args)

def trace_all_funcs(only=None):
    total_num_gen = 0
    total_num_skipped = 0

    if only:
        header_f = profile_f = None
    else:
        header_f = open("aot.h", "w")
        profile_f = open("aot_profile.c", "w")
        print("#ifndef AOT_H_", file=header_f)
        print("#define AOT_H_", file=header_f)
        print('', file=header_f)

        print_includes(header_f)

        print('#include "aot.h"', file=profile_f)

        print_helper_funcs(header_f)

        print(f"//#define DBG(...) printf(__VA_ARGS__)", file=profile_f)
        print(f"#define DBG(...)", file=profile_f)

    # list of work items we will later on trace
    async_tracing = []
    for case in cases:
        num_gen, num_skipped = specialize_func(case, header_f, profile_f, async_tracing, only=only)
        total_num_gen += num_gen
        total_num_skipped += num_skipped

    if VERBOSITY:
        list(map(do_trace, async_tracing))
    else:
        print("Starting multiprocess tracing...")
        with multiprocessing.pool.Pool() as tracing_pool:
            tracing_pool.map(do_trace, async_tracing, chunksize=1)

    print(
        f'generated in total {total_num_gen} special versions skipped {total_num_skipped}')

    if header_f:
        print("#endif", file=header_f)

def print_includes(f):
    print('#include "Python.h"', file=f)
    print('#include "opcode.h"', file=f)
    print('', file=f)

    print('#define likely(x) __builtin_expect(!!(x), 1)', file=f)
    print('#define unlikely(x) __builtin_expect(!!(x), 0)', file=f)
    print('', file=f)

    print('', file=f)

def print_helper_funcs(f):
    for func in ("PyNumber_Power", "PyNumber_InPlacePower"):
        print(f"PyObject* {func}None(PyObject *v, PyObject *w);", file=f)

    print(f"PyObject* cmp_outcome(PyThreadState *tstate, int, PyObject *v, PyObject *w);", file=f)
    for cmp in cmps:
        print(f"PyObject* cmp_outcome{cmp}(PyObject *v, PyObject *w);", file=f)

    for func in ("call_function_ceval_no_kw", "call_method_ceval_no_kw"):
        print(f"PyObject* {func}(PyThreadState *tstate, PyObject ***pp_stack, Py_ssize_t oparg);", file=f)

    print("/* this directly modifies the destination of the jit generated call instruction */\\", file=f)
    print("#define SET_JIT_AOT_FUNC(dst_addr) do { \\", file=f)
    print("    /* retrieve address of the instruction following the call instruction */\\", file=f)
    print("    unsigned int* ret_addr = (unsigned int*)__builtin_extract_return_addr(__builtin_return_address(0));\\", file=f)
    print("    /* 5 byte call instruction - get address of relative immediate operand of call */\\", file=f)
    print("    unsigned int* call_imm = &ret_addr[-1];\\", file=f)
    print("    /* set operand to newly calculated relative offset */\\", file=f)
    print("    *call_imm = (unsigned int)(unsigned long)(dst_addr) - (unsigned int)(unsigned long)ret_addr;\\", file=f)
    print("} while(0)", file=f)


def create_pre_traced_funcs(output_file):
    with open(output_file, "w") as f:
        print_includes(f)

        print("PyObject* avoid_clang_bug_aotpretrace() { return NULL; }", file=f)

        print_helper_funcs(f)

        print(f"#define Py_BUILD_CORE 1", file=f)
        print(f"#include <interp.h>", file=f)
        print(f"#include <pycore_pystate.h>", file=f)

        for num_args in range(10):
            args = ["JitTarget* target", "PyObject *func"] + \
                [f'PyObject *o{i}' for i in range(num_args)]
            print(
                f"PyObject* call_helper{num_args}(", ", ".join(args), "){", file=f)
            print("  _PyRuntimeState * const runtime = &_PyRuntime;", file=f)
            print(
                "  PyThreadState * const tstate = _PyRuntimeState_GetThreadState(runtime);", file=f)
            args = ["func"] + [f'o{i}' for i in range(num_args)] + ["NULL"]
            print(f"  PyObject* array[{num_args}+2] =",
                  "{", ", ".join(args), "};", file=f)
            print(
                f'  for (int i=0;i<({num_args}+1); ++i) Py_INCREF(array[i]);', file=f)
            print(f"  PyObject** sp = &array[{num_args+1}];", file=f)
            print(
                f'  return runJitTarget4(target, tstate, &sp, {num_args} /* oparg */, 0);', file=f)
            print('}', file=f)

        print(f"#include <interp.h>", file=f)
        for wrap in ("", "_wrap_result"):
            for num_args in range(5):
                args = ["JitTarget* target", ] + \
                    [f'PyObject *o{i}' for i in range(num_args)]
                print(
                    f"PyObject* runJitTarget{num_args}{wrap}_helper(", ", ".join(args), "){", file=f)
                args = ["target"] + [f'o{i}' for i in range(num_args)]
                ret = f"runJitTarget{num_args}({', '.join(args)})"
                if wrap:
                    ret = f"PyLong_FromLong({ret})"
                print(f'  return {ret};', file=f)
                print('}', file=f)

        for case in cases:
            case.writePretraceFunctions(f)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser("aot_gen")
    parser.add_argument("--action", action="store", default="all")
    parser.add_argument("-v", action="count", default=0, dest="verbosity")
    parser.add_argument("--only", action="store", default=None)
    parser.add_argument("-o", action="store", default=None)
    parser.add_argument("--pic", action="store_true", default=False)

    args = parser.parse_args()
    assert args.action in ("pretrace", "trace", "all")

    if args.action in ("pretrace", "all"):
        assert args.o
        create_pre_traced_funcs(args.o)

    VERBOSITY = args.verbosity

    if args.action in ("trace", "all"):
        # init JIT
        loadLibs()

        initializeJIT(VERBOSITY, args.pic)
        loadBitcode(b'all.bc')
        pystolGlobalPythonSetup()

        # start tracing
        trace_all_funcs(only=args.only)
