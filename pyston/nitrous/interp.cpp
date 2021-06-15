#include <ctime>
#include <dlfcn.h>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ffi.h>

#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "common.h"
#include "Interpreter.h"
#include "jit.h"
#include "optimization_hooks.h"
#include "symbol_finder.h"

#include "interp.h"

using namespace llvm;
using namespace std;

namespace nitrous {

static unique_ptr<SymbolFinder> symbol_finder;
static unique_ptr<LLVMCompiler> compiler;
static unique_ptr<JitConsts> jit_consts;

string findNameForAddress(void* address) {
    return symbol_finder->lookupAddress(address);
}

void* findAddressForName(const string& name) {
    return symbol_finder->lookupSymbol(name);
}

static LLVMContext context;
static const DataLayout* data_layout;

const DataLayout* getDataLayout() {
    return data_layout;
}

LLVMContext& getContext() {
    return context;
}

// Copied in from llvms ExternalFunctions.cpp
static ffi_type *ffiTypeFor(Type *Ty) {
  switch (Ty->getTypeID()) {
    case Type::VoidTyID: return &ffi_type_void;
    case Type::IntegerTyID:
      switch (cast<IntegerType>(Ty)->getBitWidth()) {
        case 8:  return &ffi_type_sint8;
        case 16: return &ffi_type_sint16;
        case 32: return &ffi_type_sint32;
        case 64: return &ffi_type_sint64;
      }
    case Type::FloatTyID:   return &ffi_type_float;
    case Type::DoubleTyID:  return &ffi_type_double;
    case Type::PointerTyID: return &ffi_type_pointer;
    default: break;
  }
  // TODO: Support other types such as StructTyID, ArrayTyID, OpaqueTyID, etc.
  report_fatal_error("Type could not be mapped for use with libffi.");
  return NULL;
}
static void *ffiValueFor(Type *Ty, const GenericValue &AV,
                         void *ArgDataPtr) {
  switch (Ty->getTypeID()) {
    case Type::IntegerTyID:
      switch (cast<IntegerType>(Ty)->getBitWidth()) {
        case 8: {
          int8_t *I8Ptr = (int8_t *) ArgDataPtr;
          *I8Ptr = (int8_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 16: {
          int16_t *I16Ptr = (int16_t *) ArgDataPtr;
          *I16Ptr = (int16_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 32: {
          int32_t *I32Ptr = (int32_t *) ArgDataPtr;
          *I32Ptr = (int32_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 64: {
          int64_t *I64Ptr = (int64_t *) ArgDataPtr;
          *I64Ptr = (int64_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
      }
    case Type::FloatTyID: {
      float *FloatPtr = (float *) ArgDataPtr;
      *FloatPtr = AV.FloatVal;
      return ArgDataPtr;
    }
    case Type::DoubleTyID: {
      double *DoublePtr = (double *) ArgDataPtr;
      *DoublePtr = AV.DoubleVal;
      return ArgDataPtr;
    }
    case Type::PointerTyID: {
      void **PtrPtr = (void **) ArgDataPtr;
      *PtrPtr = GVTOP(AV);
      return ArgDataPtr;
    }
    default: break;
  }
  // TODO: Support other types such as StructTyID, ArrayTyID, OpaqueTyID, etc.
  report_fatal_error("Type value could not be mapped for use with libffi.");
  return NULL;
}

// Reads and parses a .bc or .ll file on disk.
// Returns a parsed Module object, along with a MemoryBuffer.
// The Module object is not fully materialized, and requires
// its MemoryBuffer to be alive in order to materialize itself.
static pair<unique_ptr<Module>, unique_ptr<MemoryBuffer>> loadBitcodeFile(const char* filename) {
    ExitOnError ExitOnErr;
    ExitOnErr.setBanner(string("Error reading ") + filename + ": ");

    int len = strlen(filename);
    unique_ptr<MemoryBuffer> MB = ExitOnErr(
        errorOrToExpected(MemoryBuffer::getFileOrSTDIN(filename)));

    unique_ptr<Module> module;
    if (filename[len - 1] == 'l') {
        SMDiagnostic Err;
        bool DisableVerify = false;
        auto ModuleAndIndex = parseAssemblyFileWithIndex(
            filename, Err, context, nullptr, !DisableVerify);
        module = std::move(ModuleAndIndex.Mod);
        if (!module.get()) {
            Err.print("", errs());
            abort();
        }
    } else {
        module = ExitOnErr(getLazyBitcodeModule(*MB, context));
    }
    return make_pair(move(module), move(MB));
}

class BitcodeRegistry {
private:
    vector<unique_ptr<MemoryBuffer>> loaded_module_data;
    vector<unique_ptr<Module>> loaded_modules;
    unordered_map<string, Function*> functions;
    unordered_map<string, GlobalVariable*> global_variables;

public:
    void load(const char* filename) {
        auto module_and_buf = loadBitcodeFile(filename);
        unique_ptr<Module> &module = module_and_buf.first;

        //outs() << *module << '\n';

        for (auto& func : *module) {
            if (!func.empty() || func.isMaterializable()) {
                functions[func.getName()] = &func;
            }
        }

        for (auto& gv : module->globals()) {
            global_variables[gv.getName()] = &gv;
        }
        /*
        for (auto& gv : module->global_objects()) {
            ExitOnErr(gv.materialize());
        }
        */
        data_layout = &module->getDataLayout();

        loaded_modules.push_back(move(module));
        loaded_module_data.push_back(move(module_and_buf.second));
    }

    Function* findFunction(string name) {
        if (functions.count(name)) {
            cantFail(functions[name]->materialize());
            return functions[name];
        }
        return nullptr;
    }

    GlobalValue* findGlobalSymbol(string name) {
        Function* f = findFunction(name);
        if (f)
            return f;
        if (global_variables.count(name)) {
            cantFail(global_variables[name]->materialize());
            return global_variables[name];
        }
        return nullptr;
    }
} bitcode_registry;

const Function* functionForAddress(intptr_t address) {
    string name = findNameForAddress((void*)address);
    return bitcode_registry.findFunction(name);
}

class TraceStrategy {
public:
    bool shouldntTrace(void* addr) {
        // TODO: should have a better way of identifying these library functions
        if (addr == &printf)
            return true;
        if (addr == &labs)
            return true;
        if (addr == &strlen)
            return true;
        if (addr == &memcmp)
            return true;
        if (addr == &memmove)
            return true;
        if (addr == &clock_gettime)
            return true;
        if (addr == &malloc)
            return true;
        if (addr == &free)
            return true;
        if (addr == &_runJitTarget)
            return true;
        return false;
    }
    bool shouldTraceInto(llvm::StringRef function_name) {
        if (function_name == "PyObject_Malloc")
            return false;
        if (function_name == "_PyObject_GC_Malloc")
            return false;
        if (function_name == "PyObject_GC_Del")
            return false;
        if (function_name == "PyObject_GC_UnTrack")
            return false;
        if (function_name == "Py_FatalError")
            return false;
        if (function_name == "PyErr_Restore")
            return false;
        if (function_name == "PyErr_Format")
            return false;
        if (function_name == "_PyObject_Free")
            return false;
        if (function_name == "PyImport_ImportModuleLevelObject")
            return false;

        // A few functions that don't benefit from being inlined:
        if (function_name == "float_dealloc")
            return false;
        if (function_name == "_PyDict_GetItem_KnownHash")
            return false;
        if (function_name == "range_new")
            return false;
        if (function_name == "PyThreadState_Get")
            return false;
        if (function_name == "method_dealloc")
            return false;

        // On the fence about this one -- the argument is usually constant, which
        // is a pro for inlining, but otherwise there's a lot of gc work that
        // doesn't benefit from inlining.  More testing needed.
        // One thing I tried in the past which might be good is to just create
        // PyTuple_New1() etc functions and not inline them
        if (function_name == "PyTuple_New" || function_name == "PyTuple_New_Nonzeroed")
            return false;

        if (function_name == "builtin_print")
            return false;

        // TODO: this requires vector objects
        if (function_name == "PyDict_New")
            return false;
        // TODO this should be cut off at function calls
        if (function_name == "time_time")
            return false;

        // TODO shouldn't need these
        if (function_name == "_PyObject_GC_Alloc")
            return false;
        //if (function_name == "__Pyx_CyFunction_CallAsMethod")
            //return false;

        // _PyFrame_New_NoTrack??
        if (function_name == "_PyEval_EvalFrameDefault")
            return false;

        if (function_name.startswith("PyMem_"))
            return false;
        if (function_name == "PyObject_LengthHint")
            return false;

        if (function_name == "_PyFrame_New_NoTrack")
            return false;

        if (function_name == "float_floor_div")
            return false;

        if (function_name == "PyObject_Free")
            return false;

        if (function_name == "_PyMethodDef_RawFastCallKeywords")
            return false;

        if (function_name == "float___trunc___impl") // mixed ars
            return false;

        if (function_name == "pysiphash")
            return false;

        if (function_name == "unicode_hash" || function_name == "lookdict_unicode")
            return false;

        if (function_name == "_PyObject_LookupSpecial")
            return false;

        if (function_name == "PyLong_FromUnicodeObject" || function_name == "PyFloat_FromString")
            return false;

        if (function_name == "float_repr")
            return false;

        if (function_name == "PyErr_CheckSignals")
            return false;

        // undefined syms
        if (function_name == "abstract_issubclass" || function_name == "abstract_get_bases")
            return false;

        if (function_name == "_PyUnicodeWriter_Finish")
            return false;
        if (function_name == "l_divmod")
            return false;
        if (function_name == "PyUnicode_Format" || function_name == "unicode_mod")
            return false;
        if (function_name == "long_lshift" || function_name == "long_rshift")
            return false;
        if (function_name == "long_and" || function_name == "long_xor" || function_name == "long_or")
            return false;

        if (function_name == "complex_pow")
            return false;

        // can't find this function
        if (function_name == "unicode_new")
            return false;

        if (function_name == "lookdict")
            return false;

        if (function_name == "slice_richcompare" || function_name == "dict_subscript" || function_name == "set_subscript" || function_name == "compute_range_item")
            return false;

        if (function_name == "list_ass_slice")
            return false;

        if (function_name == "unicode_join")
            return false;

        if (function_name == "unicode_upper")
            return false;

        if (function_name == "builtin_globals")
            return false;

        // Both of these seem to make sense to not inline, since inlining doesn't
        // expose any additional opportunities.  But turning off inlining hurt
        // perf, maybe due to function call overhead?  Not sure
        //if (function_name == "frame_dealloc")
            //return false;
        //if (function_name == "_Py_CheckFunctionResult")
            //return false;

        if (function_name == "_PyObject_RealIsSubclass")
            return false;

        if (function_name == "PyUnicode_Contains")
            return false;

        if (function_name == "PyObject_RichCompareBool")
            return false;

        if (function_name == "unicode_compare")
            return false;

        if (function_name == "tupledealloc")
            return false;

        if (function_name == "PyDict_SetItem")
            return false;

        // Ideally would inline a version of this that didn't
        // have PyDict_SetItem inlined:
        if (function_name == "dict_ass_sub")
            return false;

        if (function_name == "_PyEval_SliceIndex")
            return false;

        if (function_name == "slot_tp_init")
            return false;

        if (function_name == "builtin_isinstance" || function_name == "builtin_getattr" || function_name == "builtin_len")
            return false;

        return true;
    }
};

template <typename Jit>
class Interpreter : public llvm_interp::Interpreter, public LLVMEvaluator {
private:
    Jit& jit;
    Module* module;

    llvm_interp::ExecutionContext& getExecutionContext() { return ECStack.back(); }

    bool looksLikePyObject(Type* t) {
        auto st = dyn_cast<StructType>(t);
        if (!st)
            return false;
        if (!st->getName().endswith("bject"))
            return false;

        if (st->getNumElements() >= 2 && st->getElementType(0) == Type::getInt64Ty(context)) {
            auto el1 = dyn_cast<PointerType>(st->getElementType(1));
            if (el1) {
                auto st1 = dyn_cast<StructType>(el1->getElementType());
                if (st1) {
                    if (st1->getName() == "struct._typeobject")
                        return true;
                }
            }
        }

        if (st->getNumElements() >= 1 && looksLikePyObject(st->getElementType(0)))
            return true;

        return false;
    }

    bool typesAreSimilar(Type* t1, Type* t2) {
        if (t1 == t2)
            return true;

        if (isa<PointerType>(t1) && isa<PointerType>(t2)) {
            return typesAreSimilar(cast<PointerType>(t1)->getElementType(),
                                   cast<PointerType>(t2)->getElementType());
        }

        if (isa<FunctionType>(t1) && isa<FunctionType>(t2)) {
            auto ft1 = cast<FunctionType>(t1);
            auto ft2 = cast<FunctionType>(t2);
            if (ft1->isVarArg() != ft2->isVarArg())
                return false;
            if (ft1->getNumParams() != ft2->getNumParams())
                return false;
            if (!typesAreSimilar(ft1->getReturnType(), ft2->getReturnType()))
                return false;
            for (int i = 0; i < ft1->getNumParams(); i++) {
                if (!typesAreSimilar(ft1->getParamType(i), ft2->getParamType(i)))
                    return false;
            }

            return true;
        }

        if (looksLikePyObject(t1) && looksLikePyObject(t2))
            return true;

        outs() << "Don't know how to compare types " << *t1 << " and " << *t2 << "\n";
        return false;
    }

public:
    Interpreter(Jit& jit)
        // ugly: the execution engine takes ownership of the module but we need keep ownership.
        // So we just pretend we give ownership but make sure that the EE never deletes the module.
        : llvm_interp::Interpreter(std::unique_ptr<Module>(jit.getFunction()->getParent())),
          jit(jit), module(jit.getFunction()->getParent()) {
    }
    ~Interpreter() override {
        // ugly: prevent EE from deleting the module, see comment in constructor.
        releaseOwnershipOfModule();
    }
    void releaseOwnershipOfModule() {
        if (!module)
            return;
        removeModule(module);
        module = nullptr;
    }

    void *getPointerToFunction(Function *F) override {
        return findAddressForName(F->getName());
    }

    void *getOrEmitGlobalVariable(const GlobalVariable *GV) override {
        RELEASE_ASSERT(!isa<Function>(GV), "figure out what todo");

        // We cant't use ExecutionEngine::getOrEmitGlobalVariable because
        // it will deallocate the underlying memory when the module is destroyed (=when we JIT the function)

        if (void* ret = getPointerToGlobalIfAvailable(GV))
            return ret;

        void* ret = nullptr;

        // Try to catch unnamed string constants
        auto arr_type = dyn_cast<ArrayType>(
            cast<PointerType>(GV->getType())->getElementType());
        if (!GV->hasExternalLinkage() && arr_type && arr_type->getElementType()->isIntegerTy(8)) {
            RELEASE_ASSERT(GV->isConstant(), "");

            auto initializer = GV->getInitializer();
            RELEASE_ASSERT(initializer, "");

            auto newdata_size = getDataLayout().getTypeAllocSize(initializer->getType());
            char* newdata = new char[newdata_size];
            if (auto* init = dyn_cast<ConstantDataSequential>(initializer)) {
                StringRef data = init->getRawDataValues();
                RELEASE_ASSERT(newdata_size >= data.size(), "");
                memcpy(newdata, data.data(), data.size());
            } else if (isa<ConstantAggregateZero>(initializer)) {
                // zeroinitializer
                memset(newdata, 0, newdata_size);
            } else {
                RELEASE_ASSERT(0, "not implemented");
            }
            jit.addAllocation(unique_ptr<char[]>(newdata));
            ret = newdata;
        } else
            ret = findAddressForName(GV->getName());

        RELEASE_ASSERT(ret, "");
        addGlobalMapping(GV, ret);
        return ret;
    }


    void run() override {
        while (!ECStack.empty()) {
            // Interpret a single instruction & increment the "PC".
            auto &SF = ECStack.back();  // Current stack frame
            Instruction &I = *SF.CurInst++;         // Increment before execute

            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                outs() << "Interpreting ";
                outs() << I << '\n';
            }

            visit(I);   // Dispatch to one of the visit* methods...
        }
    }

    // return true if we could sucessfully trace the function else return false
    bool traceCall(CallInst& call) {
        // don't trace intrinsics
        if (isa<Function>(call.getCalledValue()) && cast<Function>(call.getCalledValue())->isIntrinsic())
            return false;

        long addr;
        if (auto* GV = dyn_cast<GlobalValue>(call.getCalledValue()->stripPointerCasts())) {
            addr = (long)findAddressForName(GV->getName());
        } else {
            auto& SF = getExecutionContext();
            auto func = getOperandValue(call.getCalledValue(), SF);
            addr = (long)GVTOP(func);
        }

        if (TraceStrategy().shouldntTrace((void*)addr))
            return false;


        const Function* function = functionForAddress(addr);
        if (!function) {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                string name = findNameForAddress((void*)addr);
                outs() << "Can't trace '" << name << "' because we could not find the implementation\n";
            }
            return false;
        }
        RELEASE_ASSERT(function, "not a function?");

        if (function->isVarArg())
            return false;
        if (!TraceStrategy().shouldTraceInto(function->getName())) {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                outs() << "Not inlining " << function->getName() << " due to policy\n";
            return false;
        }

        RELEASE_ASSERT(!function->empty(), "need to find the right function");

        if (!isa<Constant>(call.getCalledValue())) {
            //return false; // disable funcptr tracing

            //outs() << "before splitting:\n";
            //outs() << *bb.getParent() << '\n';
            auto& bb = *call.getParent();

            auto next_bb = bb.splitBasicBlock(&call);
            auto fallback_bb = BasicBlock::Create(
                context, "", bb.getParent());
            auto inline_bb = BasicBlock::Create(
                context, "", bb.getParent(), next_bb);

            bb.getTerminator()->eraseFromParent();
            auto i64 = Type::getInt64Ty(context);
            auto bitcast = new PtrToIntInst(
                call.getCalledValue(),
                i64, "", &bb);
                /*
                pointer cmp
            auto cmp = new ICmpInst(
                bb, CmpInst::ICMP_EQ, bitcast,
                ConstantInt::get(i64, (intptr_t)addr));
                */
            module->getOrInsertFunction(function->getName(), function->getFunctionType());

            auto bitcast2 = new PtrToIntInst(
                module->getFunction(function->getName()),
                i64, "", &bb);

            auto cmp = new ICmpInst(
                bb, CmpInst::ICMP_EQ, bitcast, bitcast2);


            auto guard_br = BranchInst::Create(
                inline_bb, fallback_bb, cmp, &bb);

            MDBuilder mdbuilder(context);
            guard_br->setMetadata(
                "prof", mdbuilder.createBranchWeights(1000, 1));

            auto inline_call = cast<CallInst>(call.clone());
            inline_bb->getInstList().push_back(inline_call);
            BranchInst::Create(next_bb, inline_bb);

            auto fallback_call = call.clone();
            fallback_bb->getInstList().push_back(fallback_call);
            BranchInst::Create(next_bb, fallback_bb);

            if (!call.getType()->isVoidTy()) {
                auto phi = PHINode::Create(call.getType(), 2, "", &call);
                phi->addIncoming(inline_call, inline_bb);
                phi->addIncoming(fallback_call, fallback_bb);
                call.replaceAllUsesWith(phi);
            }

            call.eraseFromParent();

            //outs() << "after splitting:\n";
            //outs() << *bb.getParent() << '\n';

            if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
                outs() << "inlining " << function->getName() << " (from function pointer)\n";

            auto inline_result
                = jit.inlineFunction(inline_call, function);

            for (auto alloca : inline_result.StaticAllocas)
                visitAllocaInst(*alloca);

            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                outs() << "split, inlined, and resuming at "
                       << *inline_bb->begin() << '\n';
                //outs() << *bb.getParent() << '\n';
            }

            SwitchToNewBasicBlock(inline_bb, getExecutionContext());
            return true;
        }

        if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
            outs() << "inlining " << function->getName() << '\n';

        auto& bb = *call.getParent();
        auto next_bb = bb.splitBasicBlock(&call);

        auto inline_result
            = jit.inlineFunction(&call, function);

        for (auto alloca : inline_result.StaticAllocas)
            visitAllocaInst(*alloca);

        SwitchToNewBasicBlock(next_bb, getExecutionContext());
        if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
            outs() << "resuming at " << *getExecutionContext().CurInst << '\n';

        return true;
    }

    void visitCallSite(CallSite CS) override {
        if (auto* call = dyn_cast<CallInst>(CS.getInstruction())) {
            if (traceCall(*call))
                return;
        }
        llvm_interp::Interpreter::visitCallSite(CS);
    }

    void callFunction(uint64_t addr, ArrayRef<GenericValue> args) override {
        // we look at the callsite to figure out what types the arguments have.
        // imagine we call printf(str, arg1, arg2) if we only look at F(printf) we would not know what fields we set in the operand GVs
        CallSite caller = getExecutionContext().Caller;

        // I think Fast is the same as C on x86_64, and almost the same
        // on x86_32?
        // https://github.com/llvm-mirror/llvm/blob/master/lib/Target/X86/X86CallingConv.td
        RELEASE_ASSERT(caller.getCallingConv() == CallingConv::C
                       || caller.getCallingConv() == CallingConv::Fast,
                       "Trying to do a call with a different calling convention");

        RELEASE_ASSERT(args.size() == caller.getNumArgOperands(), "this should never happen");

        unsigned arg_bytes = 0;
        SmallVector<ffi_type *, 8> arg_types(args.size());
        for (int i = 0; i < caller.getNumArgOperands(); ++i) {
            Type* arg_type = caller.getArgOperand(i)->getType();
            arg_types[i] = ffiTypeFor(arg_type);
            arg_bytes += data_layout->getTypeStoreSize(arg_type);
        }

        SmallVector<uint8_t, 128> arg_data(arg_bytes);
        uint8_t* arg_data_ptr = arg_data.data();
        SmallVector<void *, 16> values(args.size());
        for (int i = 0; i < caller.getNumArgOperands(); ++i) {
            Type* arg_type = caller.getArgOperand(i)->getType();
            values[i] = ffiValueFor(arg_type, args[i], arg_data_ptr);
            arg_data_ptr += data_layout->getTypeStoreSize(arg_type);
        }

        Type* ret_type = caller.getType();
        ffi_cif cif;
        bool r = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, args.size(),
                                ffiTypeFor(ret_type), arg_types.data()) == FFI_OK;
        RELEASE_ASSERT(r, "");

        SmallVector<uint8_t, 128> ret;
        if (!ret_type->isVoidTy())
            ret.resize(data_layout->getTypeStoreSize(ret_type));

        ffi_call(&cif, (void (*)())addr, ret.data(), values.data());

        GenericValue Result;
        switch (ret_type->getTypeID()) {
        case Type::IntegerTyID:
            switch (cast<IntegerType>(ret_type)->getBitWidth()) {
            case 8:
                Result.IntVal = APInt(8, *(int8_t*)ret.data());
                break;
            case 16:
                Result.IntVal = APInt(16, *(int16_t*)ret.data());
                break;
            case 32:
                Result.IntVal = APInt(32, *(int32_t*)ret.data());
                break;
            case 64:
                Result.IntVal = APInt(64, *(int64_t*)ret.data());
                break;
            default:
                RELEASE_ASSERT(0, "");
            }
            break;
        case Type::FloatTyID:
            Result.FloatVal = *(float*)ret.data();
            break;
        case Type::DoubleTyID:
            Result.DoubleVal = *(double*)ret.data();
            break;
        case Type::PointerTyID:
            Result.PointerVal = *(void**)ret.data();
            break;
        case Type::VoidTyID:
            break;
        default:
            RELEASE_ASSERT(0, "");
        }

        // this is silly: but popStackAndReturnValueToCaller will pop a stackframe..
        ECStack.emplace_back();
        popStackAndReturnValueToCaller(ret_type, Result);
    }

    GenericValue callExternalFunction(Function *F, ArrayRef<GenericValue> args) override {
        RELEASE_ASSERT(0, "we should never get here");
    }

    void switchCallback(SwitchInst* sw, int dest) override {
        if (sw->getMetadata("prof"))
            return;

        vector<uint32_t> weights;

        weights.push_back(1 + 999 * (dest == -1));

        int idx = 0;
        for (auto& c : sw->cases()) {
            weights.push_back(1 + 999 * (dest == idx));
            idx++;
        }

        MDBuilder mdbuilder(context);
        sw->setMetadata("prof", mdbuilder.createBranchWeights(weights));
    }

    void branchCallback(BranchInst* br, int dest) override {
        // Don't overwrite existing branch-weight data.
        // I did some investigation and this seems to work best for
        // fib2 and bm_nbody, at least better than "no branch weights"
        // and "always overwrite branch weights".
        //
        // But I did an earlier investigation that found the opposite,
        // so more research is needed.
        //
        // We should probably count the branch statistics and then emit
        // a single branch weight at the end that combines all of them.
        if (br->getMetadata("prof"))
            return;

        MDBuilder mdbuilder(context);
        br->setMetadata("prof",
                        mdbuilder.createBranchWeights(1 + 999 * (dest == 0),
                                                      1 + 999 * (dest == 1)));
    }

    static GenericValue UInt64ToGV(uint64_t val, Type* type) {
        RELEASE_ASSERT(!type->isFloatingPointTy(), "this will not work");
        return PtrValToGV(&val, type);
    }

    static GenericValue PtrValToGV(void* ptr, Type* type) {
        GenericValue result;
        switch (type->getTypeID()) {
          case Type::IntegerTyID:
            switch (cast<IntegerType>(type)->getBitWidth()) {
              case 8:  result.IntVal = APInt(8 , *(int8_t *)ptr); break;
              case 16: result.IntVal = APInt(16, *(int16_t*)ptr); break;
              case 32: result.IntVal = APInt(32, *(int32_t*)ptr); break;
              case 64: result.IntVal = APInt(64, *(int64_t*)ptr); break;
            }
            break;
          case Type::FloatTyID:   result.FloatVal   = *(float *)ptr; break;
          case Type::DoubleTyID:  result.DoubleVal  = *(double*)ptr; break;
          case Type::PointerTyID: result.PointerVal = *(void **)ptr; break;
          case Type::VoidTyID: break;
          default: RELEASE_ASSERT(0, "not implemented"); break;
        }
        return result;
    }

    static uint64_t GVToUInt64(const GenericValue& val, Type* type) {
        long ret_val = 0;
        switch (type->getTypeID()) {
          case Type::IntegerTyID: ret_val = val.IntVal.getZExtValue(); break;
          case Type::FloatTyID:   RELEASE_ASSERT(0, "will not work"); break;
          case Type::DoubleTyID:  RELEASE_ASSERT(0, "will not work"); break;
          case Type::PointerTyID: ret_val = (uint64_t)val.PointerVal; break;
          case Type::VoidTyID:    ret_val = 0; break;
          default: RELEASE_ASSERT(0, "not implemented %d", type->getTypeID()); break;
        }
        return ret_val;
    }


    pair<long, void*> interpret(JitTarget* jit_target, llvm::ArrayRef<GenericValue> args) {
        Function* function = jit.getFunction();

        GenericValue result = runFunction(function, args);
        long ret_val = GVToUInt64(result, function->getReturnType());

        if (--jit_target->num_traces_until_jit <= 0) {
            // ugly: prevent EE from deleting the module, see comment in constructor.
            releaseOwnershipOfModule();

            struct timespec start, end;
            clock_gettime(CLOCK_REALTIME, &start);
            jit.optimize(*this);
            auto function_addr = jit.finish(*this);
            clock_gettime(CLOCK_REALTIME, &end);

            if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
                printf("Took %ldms to jit\n",
                       1000 * (end.tv_sec - start.tv_sec)
                           + (end.tv_nsec - start.tv_nsec) / 1000000);

            return make_pair(ret_val, function_addr);
        }
        return make_pair(ret_val, nullptr);

    }

    static pair<long, void*>
    interpret(JitTarget* jit_target, llvm::ArrayRef<long> params_long) {
        Jit*& jit = (Jit*&)jit_target->llvm_jit;
        bool first_run = !jit;
        if (first_run) {
            void* function_ptr = jit_target->target_function;
            string name = findNameForAddress(function_ptr);
            const llvm::Function* func = bitcode_registry.findFunction(name);

            if (params_long.size() != func->arg_size()) {
                llvm::outs() << *func;
                llvm::outs() << params_long.size() << " " << func->arg_size() << "\n";
            }
            RELEASE_ASSERT(params_long.size() == func->arg_size(), "");

            for (auto& arg : func->args()) {
                auto type = arg.getType();
                RELEASE_ASSERT(type->isIntegerTy() || type->isPointerTy(), "");
            }

            jit = new Jit(func, &context, compiler.get(), *jit_consts);
        }
        Interpreter<Jit> interpret(*jit);
        llvm::Function* function = interpret.jit.getFunction();
        auto function_type = function->getFunctionType();

        RELEASE_ASSERT(params_long.size() == function->arg_size(),
                       "not sure which to pass to this next line");
        RELEASE_ASSERT(params_long.size() == function_type->getNumParams(),
                       "not sure which to pass to this next line");


        llvm::SmallVector<GenericValue, 8> params;
        params.reserve(params_long.size());
        for (int i=0; i < function_type->getNumParams(); ++i)
            params.push_back(UInt64ToGV(params_long[i], function_type->getParamType(i)));

        if (first_run) {
            // apply argument flags
            llvm::Module* module = function->getParent();
            llvm::Instruction* insert_pt = &*function->getEntryBlock().getFirstInsertionPt();
            for (int arg=0; arg < jit_target->num_args; ++arg) {
                const int num_flags = 2;
                int flag = (jit_target->arg_flags >> (num_flags*arg)) & (JIT_IS_CONST|JIT_NOT_ZERO);

                // we just set nonnull on the function parameter
                if (flag & JIT_NOT_ZERO)
                    function->addParamAttr(arg, Attribute::NonNull);

                // add assume(arg == const) in function entry
                // TODO: maybe better replacing all uses with the constant instead
                if (flag & JIT_IS_CONST) {
                    llvm::Function *assume_fn = llvm::Intrinsic::getDeclaration(module, llvm::Intrinsic::assume);
                    auto* value = interpret.GVToConst(params[arg], function_type->getParamType(arg));
                    auto* icmp = new llvm::ICmpInst(insert_pt, llvm::CmpInst::Predicate::ICMP_EQ, function->getArg(arg), value);
                    insert_pt = llvm::CallInst::Create(assume_fn, icmp, "", icmp->getNextNode())->getNextNode();
                }

            }
        }

        return interpret.interpret(jit_target, params);
    }


    GenericValue loadPointer(void* ptr, llvm::Type* type) override {
        GenericValue result;
        // LoadValueFromMemory: says it takes a GenericValue* but the implementation actually expects a pointer directly to the memory location...
        LoadValueFromMemory(result, (GenericValue*)ptr, type);
        return result;
    }

    llvm::GenericValue evalConstant(const llvm::Constant* val) override {
        return getConstantValue(val);
    }

    Constant* GVToConst(const GenericValue& gv, Type* type) override {
        llvm::Constant* val;
        if (type->isPointerTy()) {
            void* ptr_val = (void*)GVToUInt64(gv, type);
            if (ptr_val == nullptr)
                return ConstantInt::getNullValue(type);

            auto name = symbol_finder->lookupAddress(ptr_val);

            // There are several constants that don't have symbol names, such as string constants,
            // type mros, etc
            if (name.empty())
                return nullptr;

            GlobalValue* gv = bitcode_registry.findGlobalSymbol(name);

            /*
             * Unfortunately dead branches can contain type mismatches.
             * For example, calling PyFloat_Type does something similar to
             *
             * if (func != &PyFloat_Type)
             *     return generic_version();
             * if (PyCFunction_Check(func))
             *     return ((PyCFunction)func)->vectorcall();
             * else
             *     return _PyObject_Vectorcall(func)
             *
             * The PyCFunction_Check branch is dead, but we will try to constant fold
             * the ->vectorcall load before we eliminate that branch.  This is to
             * a random offset inside the PyFloat_Type object and ends up resolving
             * to float_dealloc, which is not at all similar to the vectorcall type.
             */
            if (0 && !typesAreSimilar(type, gv->getType())) {
                outs() << "Looking for " << ptr_val << " of type " << *type << '\n';
                outs() << "Found " << *gv << '\n';
                RELEASE_ASSERT(false, "Type mismatch");
            }

            RELEASE_ASSERT(gv, "Couldn't find %s", name.c_str());
            RELEASE_ASSERT(gv->getName() == name, "name mismatch");
            Constant* c = jit.addGlobalReference(gv);

            if (c->getType() != type)
                return ConstantExpr::getBitCast(c, type);
            return c;
        } else if (type->isIntegerTy())
            val = ConstantInt::get(type, GVToUInt64(gv, type));
        else if (type->isDoubleTy())
            val = ConstantFP::get(type, gv.DoubleVal);
        else
            RELEASE_ASSERT(0, "type not implemented");
        return val;
    }

    static void testOptimize(Function* func, bool interpret_if_no_args) {
        Jit jit(func, &context, compiler.get(), *jit_consts);

        Interpreter<Jit> interpret(jit);

        if (interpret_if_no_args && func->arg_empty()) {
            JitTarget target({ func, 0, 0, nullptr, nullptr, 100000, 0 });
            interpret.interpret(&target, ArrayRef<GenericValue>({}));
        }

        jit.optimize(interpret);
    }
};

pair<long, void*> interpret(JitTarget* jit_target, llvm::ArrayRef<long> args) {
    return Interpreter<LLVMJit>::interpret(jit_target, args);
}

void addJitConst(char* addr, int size, int flags) {
    jit_consts->addConsts(JitConst{ addr, size, flags });
}

void addJitConsts(JitConst* consts, int num_consts) {
    jit_consts->addConsts(llvm::ArrayRef<_JitConst>{consts, static_cast<size_t>(num_consts)});
}
void addMallocLikeFunc(const char* name) {
    jit_consts->addNoAliasFunc(name);
}

} // namespace nitrous

int nitrous_verbosity;
bool nitrous_pic;

extern "C" {
void initializeJIT(int verbosity, int pic) {
    //verbosity = 1;
    nitrous_verbosity = verbosity;
    nitrous_pic = pic;

    nitrous::symbol_finder.reset(new nitrous::SymbolFinder());
    nitrous::compiler.reset(new nitrous::LLVMCompiler(nitrous::symbol_finder.get()));
    nitrous::jit_consts.reset(new nitrous::JitConsts());
}

void _loadBitcode(const char* bitcode_filename) {
    if (llvm::sys::fs::is_directory(bitcode_filename)) {
        std::error_code ec;
        llvm::sys::fs::directory_iterator it(bitcode_filename, ec);
        RELEASE_ASSERT(!ec, "%d", ec.value());

        while (it != llvm::sys::fs::directory_iterator()) {
            _loadBitcode(it->path().c_str());
            // printf("%s\n", it->path().c_str());

            it.increment(ec);
            RELEASE_ASSERT(!ec, "%d", ec.value());
        }

        return;
    }

    if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
        outs() << "Loading " << bitcode_filename << '\n';
    nitrous::bitcode_registry.load(bitcode_filename);
}

void loadBitcode(const char* bitcode_filename) {
    struct timespec start, end;
    clock_gettime(CLOCK_REALTIME, &start);
    _loadBitcode(bitcode_filename);
    clock_gettime(CLOCK_REALTIME, &end);
    printf("Took %ldms to load bitcode for %s\n",
           1000 * (end.tv_sec - start.tv_sec)
               + (end.tv_nsec - start.tv_nsec) / 1000000, bitcode_filename);
}
JitTarget* createJitTarget(void* function, int num_args, int num_traces_until_jit) {
    RELEASE_ASSERT(num_traces_until_jit >= 1, "");
    auto r = new JitTarget{ function, num_args, 0, nullptr, nullptr, num_traces_until_jit, 0 };
    //if (((LLVMJit*)r->llvm_jit)->function->getName() == "traced_01") {
    //}
    return r;
}

void setJitTargetArgFlag(JitTarget* target, int arg_num, int flag) {
    const int num_flags = 2;
    RELEASE_ASSERT(arg_num < target->num_args, "");
    RELEASE_ASSERT(arg_num < (sizeof(JitTarget::arg_flags)*8)/num_flags, "");
    RELEASE_ASSERT((flag & ~(JIT_IS_CONST | JIT_NOT_ZERO)) == 0, "unknown flag");

    target->arg_flags |= flag << (arg_num*num_flags);
}

long _runJitTarget(JitTarget* target, ...) {
    va_list vl;
    va_start(vl, target);

    if (target->jitted_trace) {
        switch (target->num_args) {
        case 0:
            return ((long (*)())target->jitted_trace)();
        case 1:
            return ((long (*)(long))target->jitted_trace)(va_arg(vl, long));
        case 2:
            return ((long (*)(long, long))target->jitted_trace)(va_arg(vl, long), va_arg(vl, long));
        case 3:
            return ((long (*)(long, long, long))target->jitted_trace)(va_arg(vl, long), va_arg(vl, long), va_arg(vl, long));
        default:
            RELEASE_ASSERT(0, "%d", target->num_args);
        }
    }

    // check if we are currently interpreting
    // this is important so that two interpreters are not modifying the same function at the same time
    if (target->currently_interpreting) {
        // even though we did not trace still count this so that recursive functions get a chance to JIT before they exit
        --target->num_traces_until_jit;
        switch (target->num_args) {
        case 0:
            return ((long (*)())target->target_function)();
        case 1:
            return ((long (*)(long))target->target_function)(va_arg(vl, long));
        case 2:
            return ((long (*)(long, long))target->target_function)(va_arg(vl, long), va_arg(vl, long));
        case 3:
            return ((long (*)(long, long, long))target->target_function)(va_arg(vl, long), va_arg(vl, long), va_arg(vl, long));
        default:
            RELEASE_ASSERT(0, "%d", target->num_args);
        }
    }

    vector<long> args;
    for (int i = 0; i < target->num_args; i++) {
        args.push_back(va_arg(vl, long));
    }
    va_end(vl);

    target->currently_interpreting = 1;
    auto r = nitrous::interpret(target, args);
    target->currently_interpreting = 0;

    target->jitted_trace = r.second;

    return r.first;
}

void optimizeBitcode(const char* function_name) {
    auto func = nitrous::bitcode_registry.findFunction(function_name);
    RELEASE_ASSERT(func, "");
    nitrous::Interpreter<nitrous::LLVMJit>::testOptimize(func, true);
}

}
