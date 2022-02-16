#ifndef _NITROUS_JIT_H
#define _NITROUS_JIT_H

#include <list>
#include <memory>
#include <vector>

// When enabled will create a single abort basic block where all abort paths will jump to.
// This is useful for manually inspecting the generated LLVM IR because it makes it much shorter.
//#define NITROUS_JIT_SINGLE_ABORT_BB

#include "llvm/Transforms/Utils/ValueMapper.h" // For ValueToValueMapTy
#include "llvm/ADT/IntervalMap.h"
#include "llvm/ExecutionEngine/GenericValue.h"

#include "common.h"
#include "optimization_hooks.h"

struct _JitTarget;

namespace llvm {
class AllocaInst;
class BasicBlock;
class CallInst;
class Constant;
class DataLayout;
class Function;
class GlobalVariable;
class ICmpInst;
class Instruction;
class LLVMContext;
class LoadInst;
class Module;
class Value;
class TargetMachine;
class Type;
namespace orc {
class ThreadSafeContext;
}
}

namespace nitrous {

struct _JitConst;
class SymbolFinder;

class LLVMJitCompiler;
class LLVMCompiler {
private:
    std::unique_ptr<LLVMJitCompiler> jit;
public:
    LLVMCompiler(SymbolFinder* finder);
    ~LLVMCompiler();

    SymbolFinder* getSymbolFinder();
    llvm::TargetMachine& getTargetMachine();

    void* compile(std::unique_ptr<llvm::Module> module, llvm::orc::ThreadSafeContext context, const std::string& funcname);
};

class JitConsts {
    llvm::IntervalMap<char*, int /* flags */>::Allocator alloc;
    llvm::IntervalMap<char*, int /* flags */> consts;

    // returns 0 if we don't know anything about the pointed to location
    int getFlags(char* ptr, int load_size);

    std::vector<std::string> no_alias_funcs;

public:
    JitConsts() : consts(alloc) {}

    bool isPointedToLocationConst(char* ptr, int load_size);
    bool isPointedToLocationNotZero(char* ptr, int load_size);
    void addConsts(llvm::ArrayRef<_JitConst> consts_array);

    const std::vector<std::string>& getNoAliasFuncs() const {
        return no_alias_funcs;
    }
    void addNoAliasFunc(llvm::StringRef name) {
        no_alias_funcs.emplace_back(name.str());
    }
};

class LLVMJit {
private:
    llvm::orc::ThreadSafeContext* llvm_context;
    LLVMCompiler* compiler;

    std::unique_ptr<llvm::Module> module;
    llvm::Function* func;
    JitConsts& consts;

    std::vector<std::unique_ptr<char[]>> allocations;

    static int num_functions;
    static std::string getUniqueFunctionName(std::string nameprefix);

    void cloneFunctionIntoAndRemap(llvm::Function* new_func,
                                   const llvm::Function* orig_func,
                                   bool remap_ref_to_self);

    void optimizeFunc(LLVMEvaluator& eval);

    void optimizeNoAliasCalls();
    llvm::Constant* optimizeConstsLoad(LLVMEvaluator& eval, llvm::LoadInst* LI);
    void optimizeConstsNotZero(LLVMEvaluator& eval);
    void optimizeConstsLoad(LLVMEvaluator& eval);


    bool doHardcodedChanges();

public:
    LLVMJit(const llvm::Function* orig_function,
            llvm::orc::ThreadSafeContext* llvm_context, LLVMCompiler* compiler,
            JitConsts& consts);

    llvm::Function* getFunction() { return func; }

    llvm::Constant* addGlobalReference(llvm::GlobalValue* gv);

    void addAllocation(std::unique_ptr<char[]> alloc) { allocations.emplace_back(std::move(alloc)); }

    struct InlineInfo {
        llvm::SmallVector<llvm::AllocaInst*, 4> StaticAllocas;
    };
    InlineInfo inlineFunction(llvm::CallInst* call,
                              const llvm::Function* calledFunction);

    void optimize(LLVMEvaluator& eval);
    void* finish(LLVMEvaluator& eval);
};

FunctionPass* createFactPass(LLVMEvaluator& eval, JitConsts& consts);

}

#endif
