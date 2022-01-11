#include <cstdio>
#include <dlfcn.h>

#include "llvm/ADT/iterator_range.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace llvm::orc;
using namespace std;

#include "common.h"
#include "interp.h"
#include "optimization_hooks.h"
#include "symbol_finder.h"

#include "jit.h"

namespace llvm {
void initializeNitrousAAWrapperPassPass(PassRegistry&);
}

namespace nitrous {

// from llvm/examples/Kaleidoscope/include/KaleidoscopeJIT.h
class LLVMJitCompiler {
private:
    SymbolFinder *finder;

public:
    using ObjLayerT = LegacyRTDyldObjectLinkingLayer;
    using CompileLayerT = LegacyIRCompileLayer<ObjLayerT, SimpleCompiler>;

    LLVMJitCompiler(SymbolFinder* _finder)
        : finder(_finder),
          Resolver(createLegacyLookupResolver(
              ES,
              [this](const std::string& Name) -> JITSymbol {
                  // Adapted from
                  // examples/Kaleidoscope/BuildingAJIT/Chapter4/KaleidoscopeJIT.h:
                  if (auto sym = ObjectLayer.findSymbol(Name, true))
                      return sym;

                  //if (auto SymAddr
                      //= RTDyldMemoryManager::getSymbolAddressInProcess(Name))
                      //return JITSymbol(SymAddr, JITSymbolFlags::Exported);
                  //return nullptr;

                  return JITSymbol((intptr_t)finder->lookupSymbol(Name),
                                   JITSymbolFlags::Exported);
              },
              [](Error Err) {
                  cantFail(std::move(Err), "lookupFlags failed");
              })),
          TM(EngineBuilder().selectTarget()),
          DL(TM->createDataLayout()),
          ObjectLayer(AcknowledgeORCv1Deprecation, ES,
                      [this](VModuleKey) {
                          return ObjLayerT::Resources{
                              std::make_shared<SectionMemoryManager>(), Resolver
                          };
                      },
                      ObjLayerT::NotifyLoadedFtor(),
                      [](uint64_t K, const object::ObjectFile& Obj,
                         const RuntimeDyld::LoadedObjectInfo& L) {
                          /* disabled because we don't link in perfjitevents if avialable
                          static JITEventListener* jit_event
                              = JITEventListener::createPerfJITEventListener();
                          if (jit_event)
                              jit_event->notifyObjectLoaded(K, Obj, L);
                          */
                      }),
          CompileLayer(AcknowledgeORCv1Deprecation, ObjectLayer, SimpleCompiler(*TM)) {
        llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    }

    TargetMachine& getTargetMachine() { return *TM; }
    SymbolFinder* getSymbolFinder() { return finder; }

    VModuleKey addModule(std::unique_ptr<Module> M) {
        auto K = ES.allocateVModule();
        cantFail(CompileLayer.addModule(K, std::move(M)));
        ModuleKeys.push_back(K);
        return K;
    }

    void removeModule(VModuleKey K) {
        ModuleKeys.erase(find(ModuleKeys, K));
        cantFail(CompileLayer.removeModule(K));
    }

    JITSymbol findSymbol(const std::string Name) {
        return findMangledSymbol(mangle(Name));
    }

private:
    std::string mangle(const std::string& Name) {
        std::string MangledName;
        {
            raw_string_ostream MangledNameStream(MangledName);
            Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
        }
        return MangledName;
    }

    JITSymbol findMangledSymbol(const std::string& Name) {
#ifdef _WIN32
        // The symbol lookup of ObjectLinkingLayer uses the
        // SymbolRef::SF_Exported
        // flag to decide whether a symbol will be visible or not, when we call
        // IRCompileLayer::findSymbolIn with ExportedSymbolsOnly set to true.
        //
        // But for Windows COFF objects, this flag is currently never set.
        // For a potential solution see: https://reviews.llvm.org/rL258665
        // For now, we allow non-exported symbols on Windows as a workaround.
        const bool ExportedSymbolsOnly = false;
#else
        const bool ExportedSymbolsOnly = true;
#endif

        // Search modules in reverse order: from last added to first added.
        // This is the opposite of the usual search order for dlsym, but makes
        // more
        // sense in a REPL where we want to bind to the newest available
        // definition.
        for (auto H : make_range(ModuleKeys.rbegin(), ModuleKeys.rend()))
            if (auto Sym
                = CompileLayer.findSymbolIn(H, Name, ExportedSymbolsOnly))
                return Sym;

        // If we can't find the symbol in the JIT, try looking in the host
        // process.
        if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
            return JITSymbol(SymAddr, JITSymbolFlags::Exported);

#ifdef _WIN32
        // For Windows retry without "_" at beginning, as RTDyldMemoryManager
        // uses
        // GetProcAddress and standard libraries like msvcrt.dll use names
        // with and without "_" (for example "_itoa" but "sin").
        if (Name.length() > 2 && Name[0] == '_')
            if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(
                    Name.substr(1)))
                return JITSymbol(SymAddr, JITSymbolFlags::Exported);
#endif

        return nullptr;
    }

    ExecutionSession ES;
    std::shared_ptr<SymbolResolver> Resolver;
    std::unique_ptr<TargetMachine> TM;
    const DataLayout DL;
    ObjLayerT ObjectLayer;
    CompileLayerT CompileLayer;
    std::vector<VModuleKey> ModuleKeys;
};


LLVMCompiler::LLVMCompiler(SymbolFinder* finder) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    jit = std::make_unique<LLVMJitCompiler>(finder);
}

LLVMCompiler::~LLVMCompiler() {
}


SymbolFinder* LLVMCompiler::getSymbolFinder() {
    return jit->getSymbolFinder();
}

TargetMachine& LLVMCompiler::getTargetMachine() {
    return jit->getTargetMachine();
}

void* LLVMCompiler::compile(unique_ptr<Module> module, string funcname) {
    jit->addModule(move(module));

    auto r = jit->findSymbol(funcname);
    RELEASE_ASSERT(r, "uh oh");
    ExitOnError ExitOnErr;
    return (void*)ExitOnErr(r.getAddress());
}

int JitConsts::getFlags(char* ptr, int load_size) {
    const int not_found_value = 0;
    if (!consts.overlaps(ptr, ptr + load_size - 1))
        return not_found_value;

    int flags = consts.lookup(ptr, not_found_value);
    for (int i = 1; i < load_size; ++i)
        RELEASE_ASSERT(consts.lookup(ptr + i, not_found_value) == flags, "flags are not the same");
    return flags;
}

void JitConsts::addConsts(llvm::ArrayRef<_JitConst> consts_array) {
    for (auto&& c : consts_array) {
        if (auto curflags = getFlags(c.addr, c.size))
            // we already have flagged this location:
            // as a sanity check make sure it's flagged the same
            RELEASE_ASSERT(c.flags == curflags, "");
        else
            consts.insert(c.addr, c.addr + c.size - 1 /* inclusive */, c.flags);
    }
}

bool JitConsts::isPointedToLocationConst(char* ptr, int load_size) {
    return getFlags(ptr, load_size) & JIT_IS_CONST;
}

bool JitConsts::isPointedToLocationNotZero(char* ptr, int load_size) {
    return getFlags(ptr, load_size) & JIT_NOT_ZERO;
}

// From Pyston:
std::string LLVMJit::getUniqueFunctionName(string nameprefix) {
    static llvm::StringMap<int> used_module_names;
    std::string name;
    llvm::raw_string_ostream os(name);
    os << nameprefix;

    // in order to generate a unique id add the number of times we encountered
    // this name to end of the string.
    auto& times = used_module_names[os.str()];
    os << '_' << ++times;
    return os.str();
}

class GVMaterializer : public ValueMaterializer {
private:
    Module* module;

public:
    GVMaterializer(Module* module) : module(module) {}

    Value* materialize(Value* v) override {
        if (Function* func = dyn_cast<Function>(v)) {
            Value* new_constant = module->getOrInsertFunction(
                func->getName(),
                cast<FunctionType>(
                    cast<PointerType>(func->getType())->getElementType())).getCallee();
            if (Function* new_func = dyn_cast<Function>(new_constant)) {
                new_func->copyAttributesFrom(func);
                new_func->setSection(func->getSection());
                new_func->setVisibility(func->getVisibility());
                new_func->setDSOLocal(func->isDSOLocal());
                new_func->setLinkage(func->getLinkage());
            }
            return new_constant;
        }

        if (GlobalVariable* gv = dyn_cast<GlobalVariable>(v)) {
            Constant* new_constant = module->getOrInsertGlobal(
                gv->getName(),
                cast<PointerType>(gv->getType())->getElementType());
            if (GlobalVariable* new_gv
                = dyn_cast<GlobalVariable>(new_constant)) {
                new_gv->copyAttributesFrom(gv);
                new_gv->setSection(gv->getSection());
                new_gv->setVisibility(gv->getVisibility());
                new_gv->setDSOLocal(gv->isDSOLocal());
                new_gv->setLinkage(gv->getLinkage());
                new_gv->setConstant(gv->isConstant());

                if (gv->isConstant() && gv->hasInitializer()) {
                    llvm::ValueToValueMapTy vmap;
                    new_gv->setInitializer(MapValue(gv->getInitializer(), vmap,
                                                    RF_None, nullptr, this));
                }
            }
            return new_constant;
        }
        return nullptr;
    }
};

void LLVMJit::cloneFunctionIntoAndRemap(Function* new_func,
                                        const Function* orig_func, bool remap_ref_to_self) {
    llvm::ValueToValueMapTy vmap;
    auto orig_arg_iter = orig_func->arg_begin();
    auto new_arg_iter = new_func->arg_begin();
    for (int i = 0; i < orig_func->arg_size(); i++) {
        vmap[&*orig_arg_iter] = &*new_arg_iter;
        orig_arg_iter++;
        new_arg_iter++;
    }
    if (remap_ref_to_self)
        vmap[orig_func] = new_func;

    SmallVector<ReturnInst*, 4> returns;
    GVMaterializer materializer(module.get());
    CloneFunctionInto(new_func, orig_func, vmap, true, returns, "", nullptr,
                      nullptr, &materializer);

    // Uncomment this to strip debug metadata from the emitted code.
    // This makes the generated IR easier to read, and may also improve compilation
    // speed a tiny amount
    //if (nitrous_verbosity < NITROUS_VERBOSITY_IR) {
        //StripDebugInfo(*module);
    //}
}

LLVMJit::LLVMJit(const Function* orig_function, LLVMContext* llvm_context,
                 LLVMCompiler* compiler, JitConsts& consts)
    : llvm_context(llvm_context),
      compiler(compiler),
      module(new llvm::Module("module", *llvm_context)),
      consts(consts) {
    module->setDataLayout(orig_function->getParent()->getDataLayout());
    module->setTargetTriple(orig_function->getParent()->getTargetTriple());

    NamedMDNode* orig_flags = orig_function->getParent()->getModuleFlagsMetadata();
    NamedMDNode* new_flags = module->getOrInsertModuleFlagsMetadata();
    for (MDNode* flag : orig_flags->operands()) {
        new_flags->addOperand(flag);
    }

    //FunctionType* ft = FunctionType::get(ret_type, arg_types, false /*vararg*/);
    FunctionType* ft = orig_function->getFunctionType();

    func = Function::Create(ft, Function::ExternalLinkage,
                            orig_function->getName() + getUniqueFunctionName("_traced"), module.get());

    // Our traced functions emit function pointer comparison with themself.
    // We want the function ptr to get compared to the traced function not the original untraced one.
    cloneFunctionIntoAndRemap(func, orig_function, true /* references to the function get remapped to the new one */);
}

LLVMJit::InlineInfo LLVMJit::inlineFunction(CallInst* call,
                                            const Function* function) {
    auto new_func = Function::Create(function->getFunctionType(),
                                     Function::ExternalLinkage,
                                     "__nitrous_inline_tmp_" + function->getName(), module.get());
    cloneFunctionIntoAndRemap(new_func, function, false /* references to the function will keep referencing the orig function */);

    //outs() << "before inlining:\n";
    //outs() << *module << '\n';
    auto orig_ft = cast<FunctionType>(
        cast<PointerType>(call->getCalledValue()->getType())->getElementType());

    auto new_ft = new_func->getFunctionType();

    if (orig_ft != new_ft) {
        SmallVector<Value*, 4> args;
        for (int i = 0; i < call->getNumArgOperands(); i++) {
            auto orig_type = orig_ft->getParamType(i);
            auto new_type = new_ft->getParamType(i);

            auto orig_arg = call->getArgOperand(i);
            if (orig_type == new_type) {
                args.push_back(orig_arg);
            } else {
                auto new_arg = new BitCastInst(orig_arg, new_type, "",
                                               /* before */ call);
                args.push_back(new_arg);
            }
        }

        auto new_call = CallInst::Create(new_func, args, "", /* before */ call);
        new_call->copyMetadata(*call);

        if (orig_ft->getReturnType() != new_ft->getReturnType()) {
            auto new_return = new BitCastInst(
                new_call, orig_ft->getReturnType(), "", /* before */ call);
            call->replaceAllUsesWith(new_return);
        } else {
            call->replaceAllUsesWith(new_call);
        }

        call->eraseFromParent();
        call = new_call;
    } else {
        call->setCalledFunction(new_func);
    }

    InlineFunctionInfo ifi;
    auto inline_result = InlineFunction(call, ifi);
    RELEASE_ASSERT((bool)inline_result, "%s", inline_result.message);

    new_func->eraseFromParent();

    Function* old_decl = module->getFunction(function->getName());
    if (old_decl && old_decl->use_empty()) {
        old_decl->eraseFromParent();
    }

    //outs() << "after inlining:\n";
    //outs() << *module << '\n';

    return { move(ifi.StaticAllocas) };
}

class NitrousAAResult : public AAResultBase<NitrousAAResult> {
public:
    static char ID; // Class identification, replacement for typeinfo
    NitrousAAResult() : AAResultBase() {
    }

    AliasResult alias(const MemoryLocation& LocA, const MemoryLocation& LocB,
                      AAQueryInfo& AAQI) {
        AliasResult base = AAResultBase::alias(LocA, LocB, AAQI);

        if (base != MayAlias)
            return base;

        auto&& extractAddrFromIntToPtr = [](const MemoryLocation& Loc, uint64_t& addr, uint64_t& size) {
            if (!isa<ConstantExpr>(Loc.Ptr) || cast<ConstantExpr>(Loc.Ptr)->getOpcode() != Instruction::IntToPtr)
                return false;
            auto* op = dyn_cast<ConstantInt>(cast<ConstantExpr>(Loc.Ptr)->getOperand(0));
            if (!op)
                return false;
            if (!Loc.Size.isPrecise())
                return false;
            addr = op->getZExtValue();
            size = Loc.Size.getValue();
            return true;
        };

        uint64_t addrA = 0, addrB = 0;
        uint64_t sizeA = 0, sizeB = 0;
        if (extractAddrFromIntToPtr(LocA, addrA, sizeA) && extractAddrFromIntToPtr(LocB, addrB, sizeB)) {
            if (addrA == addrB && sizeA == sizeB)
                return MustAlias;
            else if (addrA < addrB && addrA + sizeA <= addrB)
                return NoAlias;
            else if (addrB < addrA && addrB + sizeB <= addrA)
                return NoAlias;
            else
                return PartialAlias;
        }

        return MayAlias;
    }
};

/// Legacy wrapper pass to provide the BasicAAResult object.
class NitrousAAWrapperPass : public ImmutablePass {
  std::unique_ptr<NitrousAAResult> Result;

  virtual void anchor();

public:
  static char ID;

  NitrousAAWrapperPass();

  NitrousAAResult &getResult() { return *Result; }
  const NitrousAAResult &getResult() const { return *Result; }

  bool doInitialization(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

NitrousAAWrapperPass::NitrousAAWrapperPass() : ImmutablePass(ID) {
    initializeNitrousAAWrapperPassPass(*PassRegistry::getPassRegistry());
}

char NitrousAAWrapperPass::ID = 0;

void NitrousAAWrapperPass::anchor() {}

bool NitrousAAWrapperPass::doInitialization(Module &M) {
  Result.reset(new NitrousAAResult());

  return false;
}

void NitrousAAWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

} // namespace nitrous

using namespace nitrous;

INITIALIZE_PASS_BEGIN(NitrousAAWrapperPass, "nitrousaa",
                      "Nitrous Alias Analysis", false, true)
INITIALIZE_PASS_END(NitrousAAWrapperPass, "nitrousaa",
                    "Nitrous Alias Analysis", false, true)

namespace nitrous {

// doHardcodedChanges: a hacky way of hardcoding certain optimizations
// to happen.  This is for seeing what the benefit would be of certain
// optimizations without having to properly implement them.
//
// It works by outputting an index per instruction, which can then
// be used to reference the instruction from doHardcodedChanges.
// For example, to pretend that we are able to know that the condition
// evaluated by instruction 7 is always false, we can do:
//    instructions[7]->replaceAllUsesWith(
//        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
//
// To actually run this, uncomment the call in optimizeFunc()
bool LLVMJit::doHardcodedChanges() {
    unordered_map<Instruction*, int> inst_ids;
    vector<Instruction*> instructions;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            int id = inst_ids.size();
            inst_ids[&inst] = id;
            instructions.push_back(&inst);
        }
    }

    for (auto& bb : *func) {
        outs() << '\n';
        bb.printAsOperand(outs(), false);
        outs() << bb.getName() << '\n';
        for (auto& inst : bb) {
            outs() << inst_ids[&inst] << ": " << inst << '\n';
        }
    }

// simple_add3:
#if 0
    instructions[24]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
    instructions[69]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
    instructions[212]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
    instructions[229]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
#endif

// This is the code for manually optimizing simple_count:
#if 0
    // Checking if the conditional is None/True/False
    auto sw = cast<SwitchInst>(instructions[7]);
    auto br = BranchInst::Create(sw->getDefaultDest(), sw->getParent());
    sw->eraseFromParent();

    // Checking if conditional->tp_as_number is null
    instructions[17]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));

    // Knowing that conditional->tp_as_number->nb_bool is float_bool
    instructions[21]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt64Ty(*llvm_context), 4419200));

    // Knowing that the loop variable is a float
    instructions[63]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt64Ty(*llvm_context), 8780736));

    // Knowing that loop_var->tp_dealloc is float_dealloc
    instructions[148]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 1));

    // Knowing that loop_var is a float
    instructions[150]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 1));

    // Knowing that there's always space on the float freelist
    //instructions[153]->replaceAllUsesWith(
        //ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));

    // Knowing that loop_var->tp_dealloc is float_dealloc
    instructions[191]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 1));

    // Knowing that loop_var is a float
    instructions[193]->replaceAllUsesWith(
        ConstantInt::get(Type::getInt1Ty(*llvm_context), 1));

    // Knowing that there's always space on the float freelist
    //instructions[196]->replaceAllUsesWith(
        //ConstantInt::get(Type::getInt1Ty(*llvm_context), 0));
#endif


    return true;
}

struct RemoveICmpPtrPass : public FunctionPass {
    static char ID;

    LLVMEvaluator& eval;
    int num_replaced = 0;

    RemoveICmpPtrPass(LLVMEvaluator& eval) : FunctionPass(ID), eval(eval) {}
    ~RemoveICmpPtrPass() {
        if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
            outs() << "Removed " << num_replaced << " noalias_call() == const pointer comparisons\n";
    }

    static bool isInteresting(Value* v) {
        v = v->stripPointerCasts();

        if (Constant* C = dyn_cast<Constant>(v))
            return !C->isNullValue(); // null checks are not interesting

        if (isNoAliasCall(v))
            return true;

        if (PtrToIntInst* ptoi = dyn_cast<PtrToIntInst>(v))
            return isInteresting(ptoi->getOperand(0));

        if (PHINode* phi = dyn_cast<PHINode>(v))
            return all_of(phi->operand_values(), isInteresting);

        return false;
    }

    bool runOnFunction(Function &F) override {
        bool changed = false;
        for (auto&& BB : F)
            changed |= runOnBasicBlock(BB);
        return changed;
    }

    bool runOnBasicBlock(BasicBlock &BB) {
        bool modified = false;
        for (auto it = BB.begin(); it != BB.end();) {
            auto* icmp = dyn_cast<ICmpInst>(&*it++);
            if (!icmp || !icmp->isEquality())
                continue;

            auto* op0 = icmp->getOperand(0)->stripPointerCasts();
            auto* op1 = icmp->getOperand(1)->stripPointerCasts();

            // check if we can handle this icmp
            // not all optimizations for 'interesting' icmps are implemented currently
            // we will assert if we hit a possible missed optimization
            if (!(isInteresting(op0) && isInteresting(op1)))
                continue;

            auto* ptr_constant = dyn_cast<Constant>(op1);
            if (!ptr_constant) {
                RELEASE_ASSERT(0, "investigate if we can handle this case");
                continue;
            }

            void* ptr = GVTOP(eval.evalConstant(ptr_constant));
            if (!ptr || !isNoAliasCall(op0)) {
                RELEASE_ASSERT(0, "investigate if we can handle this case");
                continue;
            }

            auto eq_pred = icmp->getPredicate() == ICmpInst::Predicate::ICMP_EQ;
            Constant* c = eq_pred ? ConstantInt::getFalse(BB.getContext()) : ConstantInt::getTrue(BB.getContext());
            icmp->replaceAllUsesWith(c);
            it = icmp->eraseFromParent();

            modified = true;
            ++num_replaced;
        }
        return modified;
    }
};
char RemoveICmpPtrPass::ID = 0;

vector<function<FunctionPass*(LLVMEvaluator& eval)>> pass_factories;
void registerPassFactory(function<FunctionPass*(LLVMEvaluator& eval)> factory) {
    pass_factories.push_back(move(factory));
}

void LLVMJit::optimizeFunc(LLVMEvaluator& eval) {
    llvm::legacy::FunctionPassManager fpm(module.get());

    //fpm.add(new DataLayoutPass());
    //fpm.add(createBasicAliasAnalysisPass());
    //fpm.add(createTypeBasedAliasAnalysisPass());

    fpm.add(new NitrousAAWrapperPass());
    auto AnalysisCallback = [](Pass& P, Function&, AAResults& AAR) {
        if (auto* WrapperPass = P.getAnalysisIfAvailable<NitrousAAWrapperPass>()) {
            AAR.addAAResult(WrapperPass->getResult());
        } else {
            RELEASE_ASSERT(0, "didn't find AA pass");
        }
    };
    fpm.add(createExternalAAWrapperPass(AnalysisCallback));

    if (0) {
        fpm.add(createInstructionCombiningPass());
        fpm.add(createReassociatePass());
        fpm.add(createGVNPass());
    } else {
        fpm.add(llvm::createEarlyCSEPass());                   // Catch trivial redundancies
        fpm.add(llvm::createJumpThreadingPass());              // Thread jumps.
        fpm.add(llvm::createCorrelatedValuePropagationPass()); // Propagate conditionals
        fpm.add(llvm::createCFGSimplificationPass());          // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass());       // Combine silly seq's

        fpm.add(llvm::createTailCallEliminationPass()); // Eliminate tail calls
        fpm.add(llvm::createCFGSimplificationPass());   // Merge & remove BBs
        fpm.add(llvm::createReassociatePass());         // Reassociate expressions
        fpm.add(llvm::createLoopRotatePass());          // Rotate Loop
        fpm.add(llvm::createLICMPass());                // Hoist loop invariants
        fpm.add(llvm::createLoopUnswitchPass(true /*optimize_for_size*/));
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createIndVarSimplifyPass()); // Canonicalize indvars
        fpm.add(llvm::createLoopIdiomPass());      // Recognize idioms like memset.
        fpm.add(llvm::createLoopDeletionPass());   // Delete dead loops

        fpm.add(llvm::createLoopUnrollPass()); // Unroll small loops

        fpm.add(llvm::createGVNPass());       // Remove redundancies
        fpm.add(llvm::createMemCpyOptPass()); // Remove memcpy / form memset
        fpm.add(llvm::createSCCPPass());      // Constant prop with SCCP

        // Run instcombine after redundancy elimination to exploit opportunities
        // opened up by them.
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createJumpThreadingPass()); // Thread jumps
        fpm.add(llvm::createCorrelatedValuePropagationPass());
        fpm.add(llvm::createDeadStoreEliminationPass()); // Delete dead stores

        fpm.add(createFactPass(eval, consts));
        fpm.add(llvm::createInstructionCombiningPass());
        //fpm.add(new RemoveICmpPtrPass(eval));

        for (auto& factory : pass_factories) {
            fpm.add(factory(eval));
            fpm.add(llvm::createInstructionCombiningPass());
        }

        fpm.add(llvm::createLoopRerollPass());
        // fpm.add(llvm::createSLPVectorizerPass());   // Vectorize parallel scalar chains.


        fpm.add(llvm::createAggressiveDCEPass());        // Delete dead instructions
        fpm.add(llvm::createCFGSimplificationPass());    // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass()); // Clean up after everything.

        // fpm.add(llvm::createBarrierNoopPass());
        // fpm.add(llvm::createLoopVectorizePass(DisableUnrollLoops, LoopVectorize));
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createCFGSimplificationPass());

        fpm.add(llvm::createDeadStoreEliminationPass());
    }

    fpm.doInitialization();

    bool changed = fpm.run(*func);

    // Enable this block to do manual hardcoded optimizations:
    if (0) {
        changed = doHardcodedChanges();
        if (changed)
            fpm.run(*func);
    }
}

Constant* LLVMJit::optimizeConstsLoad(LLVMEvaluator &eval, LoadInst* I) {
    auto* C = dyn_cast<Constant>(I->getPointerOperand());
    if (!C)
        return nullptr;

    auto* type = I->getType();
    char* ptr = (char*)GVTOP(eval.evalConstant(C));
    auto& DL = module->getDataLayout();
    long size = DL.getTypeStoreSize(type);
    if (!consts.isPointedToLocationConst(ptr, size))
        return nullptr;
    Constant* c = eval.GVToConst(eval.loadPointer(ptr, type), type);
    if (!c && nitrous_verbosity > NITROUS_VERBOSITY_IR) {
        outs() << "weren't able to optimize this:\n" << *I << '\n';
    }
    return c;
}

void LLVMJit::optimizeConstsNotZero(LLVMEvaluator &eval) {
    auto&& isNotZero = [&](Value* ptr_operand, Type* value_type) {
        // we can only handle constant pointers
        auto* C = dyn_cast<Constant>(ptr_operand);
        if (!C)
            return false;

        char* ptr = (char*)GVTOP(eval.evalConstant(C));
        auto& DL = module->getDataLayout();
        long size = DL.getTypeStoreSize(value_type);

        // we don't add assumes if the pointed to value is constant
        if (consts.isPointedToLocationConst(ptr, size))
            return false;

        return consts.isPointedToLocationNotZero(ptr, size);
    };


    // find all loads and stores where we know the value stored at the pointed to location is not zero
    // and put the load inst (for loads) and the stored value (for stores) into a list so we can add
    // assume calls later.
    SmallVector<Instruction*, 8> not_zero_instrs;
    for (auto&& I : instructions(*func)) {
        if (auto* SI = dyn_cast<StoreInst>(&I)) {
            auto* VI = dyn_cast<Instruction>(SI->getValueOperand());
            if (VI && isNotZero(SI->getPointerOperand(), VI->getType()))
                not_zero_instrs.push_back(VI);
        } else if (auto* LI = dyn_cast<LoadInst>(&I)) {
            if (isNotZero(LI->getPointerOperand(), LI->getType()))
                not_zero_instrs.push_back(LI);
        }
    }

    // add '__builtin_assume(<intr> != 0)' for every instruction we found
    for (auto&& not_zero : not_zero_instrs) {
        Function *assume_fn = Intrinsic::getDeclaration(module.get(), Intrinsic::assume);
        auto* zero_value =  ConstantInt::getNullValue(not_zero->getType());
        auto* icmp = new ICmpInst(not_zero->getNextNode(), ICmpInst::Predicate::ICMP_NE, not_zero, zero_value);
        llvm::CallInst::Create(assume_fn, icmp, "", icmp->getNextNode());
    }

    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
        outs() << "Added " << not_zero_instrs.size() << " load/store != 0 assumes\n";
}

void LLVMJit::optimizeNoAliasCalls() {
    for (auto&& func_name : consts.getNoAliasFuncs()) {
        if (auto* func = module->getFunction(func_name)) {
            func->setReturnDoesNotAlias();
            // just setting the attribute on the function declaration is not enough we
            // have to update all callsites.
            for (auto&& U : func->users()) {
                if (auto* call = dyn_cast<CallInst>(U))
                    call->addAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
            }
        }
    }
}

void LLVMJit::optimizeConstsLoad(LLVMEvaluator& eval) {
    int num_gv_replaced = 0;
    int num_instr_simplified = 0;
    int num_instr_deleted = 0;
    int num_bb_cfg_simplified = 0;
    int num_iterations = 0;

    int num_total_instr = func->getInstructionCount();

    llvm::SmallVector<std::pair<WeakVH, Constant*>, 8> replace_later;
    auto&& TTI = compiler->getTargetMachine().getTargetTransformInfo(*func);
    /*
    // replace uses of globals with the address of the symbol
    // this allows llvm for example to remove pointer comparisons.
    for (auto&& GL : module->getGlobalList()) {
        if (GL.use_empty())
            continue;
        auto* v = eval.GVToConst(eval.evalConstant(&GL), GL.getType());
        // we can't use GL.uses() because we are modifying the use list!
        Use* next_use;
        for (Use* U = &*GL.use_begin(); U; U = next_use) {
            next_use = U->getNext();
            U->set(v);
            ++num_gv_replaced;
        }
    }*/

    bool modified;
    do {
        ++num_iterations;
        modified = false;

        // find const loads and remember concrete value
        for (auto&& I : instructions(*func)) {
            auto* LI = dyn_cast<LoadInst>(&I);
            if (!LI)
                continue;

            if (Constant* C = optimizeConstsLoad(eval, LI))
                replace_later.emplace_back(std::make_pair(LI, C));
        }

        // replace the const loads with the constant and recursively simplify
        for (auto&& repl : replace_later) {
            auto* I = dyn_cast_or_null<Instruction>(repl.first);
            if (!I) // looks like the instruction got already destroyed
                continue;

            modified = true;
            ++num_instr_deleted;

            if (replaceAndRecursivelySimplify(I, repl.second))
                ++num_instr_simplified;
        }


        if (modified) {
            // this optimization often causes conditional branches to be become constant
            // which allowes the CFG to be simplified a lot --> run simplifyCFG early
            // because it may add additional optimization opertunities and removes dead code (=less work todo).
            for (auto it = func->begin(); it != func->end();) {
                // we could do call 'ConstantFoldTerminator'
                // but simplifyCFG is even more powerful because it will merge blocks and erase dead ones
                // NOTE: this also seems to invalidate the iterator / llvm does it the same way...
                if (simplifyCFG(&*it++, TTI))
                    ++num_bb_cfg_simplified;
            }
        }
        replace_later.clear();
    } while (modified); // repeat until we fail to simplify anything

    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS) {
        auto instr_erased = num_total_instr - func->getInstructionCount();
        outs() << "Optimize const loads run " << num_iterations << " times and erased in total " << instr_erased  << " instructions:\n";
        outs() << "  replaced " << num_gv_replaced << " GV uses with addresses\n";
        outs() << "  erased " << num_instr_deleted << " instructions directly\n";
        outs() << "  simplified " << num_instr_simplified << " instructions\n";
        outs() << "  simplified the CFG of " << num_bb_cfg_simplified << " blocks\n";
    }
}

class DebugInfoPrinter {
private:
    string path_prefix;

    StringRef last_filename;
    int last_line = 0;

    // TODO: cache source files
    string getSource(StringRef filename, int line) {
        ErrorOr< unique_ptr< MemoryBuffer > > buf(nullptr);
        string real_filename(filename);
        if (filename.startswith("../..")) {
            real_filename = ("build/Release/" + filename).str();
        }
        if (real_filename[0] != '/')
            real_filename = path_prefix + real_filename;
        buf = MemoryBuffer::getFile(real_filename);

        if (error_code ec = buf.getError())
            return "Could not read file " + real_filename + ": " + ec.message();

        line_iterator it(**buf);

        while (it.line_number() < line) {
            ++it;
            if (it.is_at_end())
                return "failed to find line";
        }
        if (it.line_number() != line)
            return "failed to find line";

        return it->str();
    }

    void _printInliningInfo(DILocation* loc, bool arrow) {
        if (!loc)
            return;

        MDNode* scope = loc->getScope();
        DISubprogram* subprogram = cast<DILocalScope>(scope)->getSubprogram();
        if (arrow)
            outs() << " <-";
        outs() << ' ' << subprogram->getName();

        _printInliningInfo(loc->getInlinedAt(), true);
    }

public:
    DebugInfoPrinter() {
        // Try to find the path to the nitrous directory
        if (sys::fs::exists("../../pyston/nitrous") && sys::fs::exists("../../pyston/pystol"))
            path_prefix = "../../";
    }

    void printInliningInfo(DILocation* loc) {
        if (!loc)
            return;

        outs() << "; ";
        _printInliningInfo(loc, false);
        outs() << '\n';
    }

    void printDebugInfo(Instruction& I) {
        DILocation *loc = I.getDebugLoc();
        if (!loc)
            return;

        StringRef filename = loc->getFilename();
        int line = loc->getLine();

        if (filename == last_filename && line == last_line)
            return;

        if (line == 0)
            return;

        printInliningInfo(loc);

        last_filename = filename;
        last_line = line;

        outs() << "; " << filename << ":" << line << '\n';
        string source = getSource(filename, line);
        if (!source.empty())
            outs() << "; " << source << '\n';
    }
};

void verbosePrint(Function* func) {
    //outs() << *module << '\n';

    //outs() << *func << '\n';
    outs() << "{\n";
    for (auto& bb : *func) {
        DebugInfoPrinter printer;

        outs() << '\n';
        bb.printAsOperand(outs(), false);
        outs() << "\tpreds: ";
        auto PI = pred_begin(&bb), PE = pred_end(&bb);
        while (PI != PE) {
            (*PI)->printAsOperand(outs(), false);
            PI++;
            outs() << " ";
        }
        outs() << "\n";
        for (auto& inst : bb) {
            if (auto *II = dyn_cast<IntrinsicInst>(&inst)) {
                switch (II->getIntrinsicID()) {
                    case Intrinsic::dbg_value:
                        continue;
                }
            }

            printer.printDebugInfo(inst);
            outs() << inst << '\n';
        }
    }
    outs() << "}\n";
}

void LLVMJit::optimize(LLVMEvaluator& eval) {
    // kmod: I find the generated BB names to be very hard to read,
    // since they are long and very similar to each other.
    // Remove them so it falls back to numeric naming.
    for (auto& bb : *func)
        bb.setName("");

    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
        outs() << "pre-optimized:\n";
        //outs() << *func << '\n';
        verbosePrint(func);
    }

    struct timespec start, end;
    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
        clock_gettime(CLOCK_REALTIME, &start);

    optimizeNoAliasCalls();
    optimizeConstsLoad(eval);
    optimizeConstsNotZero(eval);

    optimizeFunc(eval);

    optimizeConstsLoad(eval);
    optimizeConstsNotZero(eval);
    optimizeFunc(eval);

    optimizeConstsLoad(eval);
    optimizeConstsNotZero(eval);
    optimizeFunc(eval);

    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS) {
        clock_gettime(CLOCK_REALTIME, &end);
        printf("Took %ldms to optimize\n", 1000 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000);
    }

    // Remove BB names again since the optimizer can add new ones,
    // and they're all called ".thread<N>"
    for (auto& bb : *func)
        bb.setName("");

    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
        outs() << "optimized:\n" << '\n';
        verbosePrint(func);
    }

    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS) {
        outs() << "NUM INST: " << func->getInstructionCount() << "\n";
        outs() << "NUM BB  : " << func->size() << "\n";
    }
}

void* LLVMJit::finish(LLVMEvaluator& eval) {
    static int a = 0;
    func->setName(func->getName().slice(0, func->getName().find("_traced")));
    std::error_code EC;
    std::string file_name = ("aot_module." + func->getName() + ".bc").str();
    llvm::raw_fd_ostream OS(file_name, EC, llvm::sys::fs::F_None);
    llvm::Module* mod = func->getParent();
    for (auto&& f : mod->globals()) {
        if (nitrous_pic)
            f.setDSOLocal(false); // this is like -fPIC
        if (f.isMaterializable()) {
            f.setLinkage(llvm::GlobalValue::PrivateLinkage);
            continue;
        }
        if (!f.isDeclaration())
            f.setLinkage(llvm::GlobalValue::PrivateLinkage);
        else
            f.setLinkage(llvm::GlobalValue::ExternalLinkage);
    }

    for (auto&& f : mod->functions()) {
        if (nitrous_pic)
            f.setDSOLocal(false); // this is like -fPIC
        if (!f.isDeclaration())
            continue;
        f.setLinkage(llvm::GlobalValue::ExternalLinkage);
    }

    RELEASE_ASSERT(!verifyModule(*module, &errs()),
                   "module failed to verify");

    llvm::WriteBitcodeToFile(*mod, OS);
    OS.flush();

    struct timespec start, end;
    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
        clock_gettime(CLOCK_REALTIME, &start);
    auto r = compiler->compile(move(module), func->getName());

    if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS) {
        clock_gettime(CLOCK_REALTIME, &end);
        printf("Took %ldms to compile\n", 1000 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000);
    }

    outs().flush();

    return r;
}

Constant* LLVMJit::addGlobalReference(GlobalValue* gv) {
    if (isa<Function>(gv))
        return cast<Constant>(module->getOrInsertFunction(gv->getName(), cast<FunctionType>(gv->getType()->getElementType())).getCallee());
    return module->getOrInsertGlobal(gv->getName(), gv->getType()->getElementType());
}

} // namespace nitrous
