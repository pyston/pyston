#include <vector>
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Instructions.h"

namespace pystol {

class BlockMatcher {
    /* BlockMatcher:
     * Computes whether two basic blocks are "matched".
     * Blocks A and B are matched if the execution occurrences
     * of A and B match the pattern (AB)*
     * ie they alternate, with A starting first, and ending with B
     * Note that this relationship is not symmetric.
     *
     * If there are blocks ending with an "unreachable" instruction,
     * the pattern is allowed to be (AB)*AX where X is the block ending
     * with unreachable.
     *
     * In terms of implementation, the "matched" relationship
     * is broken down into the intersection of two propreties.
     * In the string of A and B occurrences:
     * - B can only come after an A (not after a B or at the beginning)
     * - After each A must be a B (not an A or the end)
     *
     * Together, these show that the string must match (AB)*
     * They are the same property about A and B but reversed
     *
     * Concretely, "B can only come after an A" is equivalent to
     * "B and the entry block have no paths to B that don't go
     * through A".
     * Similarly, "After each A must be a B" is equivalent to
     * "A has no paths to A or an exit block that doesn't go through B"
     */
private:
    llvm::Function* func;
    typedef llvm::SmallVector<llvm::BasicBlock*, 4> BBVector;

    BBVector exit_blocks;

    // Returns whether there is a path from any block in `from` to any
    // block in `to` without going through `without`.
    // The path is considered to be coming out of `from` and needs to go in
    // to `to`.  In particular this means that if there is a shared element
    // in the two vectors, it doesn't automatically count as a path.
    bool canReach(const BBVector& from, const BBVector& to, const BBVector& without);

public:
    BlockMatcher(llvm::Function* func) : func(func) {
        for (auto &BB : *func) {
            if (succ_empty(&BB) && !llvm::isa<llvm::UnreachableInst>(BB.getTerminator()))
                exit_blocks.push_back(&BB);
        }
    }

    // `avoid` consists of blocks that are to be ignored from the CFG.
    // It should only be used in specific circumstances
    bool blocksAreMatched(llvm::BasicBlock* A, llvm::BasicBlock* B,
                          llvm::BasicBlock* avoid = nullptr);

    // Returns the list of instructions that could be executed between an instruction A and instruction B.
    // Makes the most sense if A->getParent() and B->getParent() are matched blocks.
    static std::vector<llvm::Instruction*>
    instructionsBetween(llvm::Instruction* A, llvm::Instruction* B,
                        llvm::BasicBlock* avoid = nullptr);
};

bool isNamedStructPointer(llvm::Type* t, const char* name);

// A pass to identify and remove unnecessary recursion checks.
// Recursion checks are a bit tricky to identify, since they consist
// of ~10 instructions on either side.  Right now we try to match
// the specific instruction pattern that clang emits.
// Here's an example of what the recursion check pair looks like:
/*
define %struct._object* @cmp_outcomePyCmp_EQFloatFloat2(%struct._object* nocapture readonly %0, %struct._object* nocapture readonly %1, %struct.AOTFunc* nocapture readnone %2) local_unnamed_addr #0 {
  %4 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %5 = inttoptr i64 %4 to %struct._ts*
  %6 = getelementptr inbounds %struct._ts, %struct._ts* %5, i64 0, i32 4
  %7 = load i32, i32* %6, align 8, !tbaa !4
  %8 = add i32 %7, 1
  store i32 %8, i32* %6, align 8, !tbaa !4
  %9 = load i32, i32* @_Py_CheckRecursionLimit, align 4, !tbaa !12
  %10 = icmp sgt i32 %8, %9
  br i1 %10, label %11, label %15, !prof !13

11:                                               ; preds = %3
  %12 = tail call i32 @_Py_CheckRecursiveCall(i8* getelementptr inbounds ([15 x i8], [15 x i8]* @.str.25.4573, i64 0, i64 0)) #3
  %13 = icmp eq i32 %12, 0
  br i1 %13, label %14, label %47

14:                                               ; preds = %11
  %.pre = load i32, i32* @_Py_CheckRecursionLimit, align 4, !tbaa !12
  br label %15

15:                                               ; preds = %14, %3
  %16 = phi i32 [ %.pre, %14 ], [ %9, %3 ]
  %17 = getelementptr inbounds %struct._object, %struct._object* %0, i64 1
  %18 = bitcast %struct._object* %17 to double*
  %19 = load double, double* %18, align 8, !tbaa !14
  %20 = getelementptr inbounds %struct._object, %struct._object* %1, i64 1
  %21 = bitcast %struct._object* %20 to double*
  %22 = load double, double* %21, align 8, !tbaa !14
  %23 = fcmp une double %19, %22
  %24 = select i1 %23, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0, i32 0), i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0, i32 0)
  %25 = select i1 %23, %struct._object* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0), %struct._object* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0)
  %26 = load i64, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_FalseStruct, i64 0, i32 0, i32 0, i32 0), align 8
  %27 = icmp ne i64 %26, 0
  tail call void @llvm.assume(i1 %27)
  %28 = load i64, i64* getelementptr inbounds (%struct._longobject, %struct._longobject* @_Py_TrueStruct, i64 0, i32 0, i32 0, i32 0), align 8
  %29 = icmp ne i64 %28, 0
  tail call void @llvm.assume(i1 %29)
  %30 = select i1 %23, i64 %26, i64 %28
  %31 = add i64 %30, 1
  store i64 %31, i64* %24, align 8, !tbaa !18
  %32 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %33 = inttoptr i64 %32 to %struct._ts*
  %34 = getelementptr inbounds %struct._ts, %struct._ts* %33, i64 0, i32 4
  %35 = load i32, i32* %34, align 8, !tbaa !4
  %36 = add i32 %35, -1
  store i32 %36, i32* %34, align 8, !tbaa !4
  %37 = icmp sgt i32 %16, 200
  %38 = add i32 %16, -50
  %39 = ashr i32 %16, 2
  %40 = mul nsw i32 %39, 3
  %41 = select i1 %37, i32 %38, i32 %40
  %42 = icmp slt i32 %36, %41
  br i1 %42, label %43, label %47, !prof !19

43:                                               ; preds = %15
  %44 = load atomic i64, i64* getelementptr inbounds (%struct.pyruntimestate, %struct.pyruntimestate* @_PyRuntime, i64 0, i32 12, i32 1, i32 0) monotonic, align 8
  %45 = inttoptr i64 %44 to %struct._ts*
  %46 = getelementptr inbounds %struct._ts, %struct._ts* %45, i64 0, i32 5
  store i8 0, i8* %46, align 4, !tbaa !20
  br label %47

47:                                               ; preds = %43, %15, %11
  %48 = phi %struct._object* [ null, %11 ], [ %25, %43 ], [ %25, %15 ]
  ret %struct._object* %48
}
*/
class RemoveRecursionChecksPass : public llvm::FunctionPass {
private:
    int num_removed = 0;

    struct RecursionEnterCheck {
        llvm::StoreInst* incdepth;
        llvm::BasicBlock* exc_block;
        llvm::BranchInst* overflow_br;
    };

    struct RecursionLeaveCheck {
        llvm::StoreInst* decdepth;
        llvm::BasicBlock* underflow_block;
        llvm::BranchInst* underflow_br;
    };

    bool isThreadStatePointerLoad(llvm::Value* v);

    template <typename T>
    bool isThreadStatePointer(llvm::Value* v, T& output) {
        auto cast = llvm::dyn_cast<llvm::IntToPtrInst>(v);
        if (!cast)
            return false;

        if (!isNamedStructPointer(cast->getType(), "struct._ts"))
            return false;

        return isThreadStatePointerLoad(cast->getOperand(0));
    }

    template <typename T>
    bool isRecursionDepthPointer(llvm::Value* v, T& output) {
        auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v);
        if (!gep)
            return false;

        if (!gep->hasAllConstantIndices() || gep->getNumIndices() != 2)
            return false;

        int i = 0;
        for (auto &idx : gep->indices()) {
            if (i == 0 && llvm::cast<llvm::ConstantInt>(idx)->getSExtValue() != 0)
                return false;
            if (i == 1 && llvm::cast<llvm::ConstantInt>(idx)->getSExtValue() != 4)
                return false;
            i++;
        }
        return isThreadStatePointer(gep->getPointerOperand(), output);
    }

    template <typename T>
    bool isRecursionDepthLoad(llvm::Value* v, T& output) {
        auto load = llvm::dyn_cast<llvm::LoadInst>(v);
        if (!load)
            return false;
        return isRecursionDepthPointer(load->getPointerOperand(), output);
    }

    template <typename T>
    bool isRecursionDepthIncrement(llvm::Value* v, int amount, T& output) {
        auto add = llvm::dyn_cast<llvm::BinaryOperator>(v);
        if (!add)
            return false;
        if (add->getOpcode() != llvm::Instruction::Add)
            return false;

        auto op1 = llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(1));
        if (!op1)
            return false;
        if (op1->getSExtValue() != amount)
            return false;
        return isRecursionDepthLoad(add->getOperand(0), output);
    }

    bool isRecursionEnter(llvm::Instruction* inst, RecursionEnterCheck& output);

    bool isRecursionLeave(llvm::Instruction* inst, RecursionLeaveCheck& output);

    static llvm::StringSet<> guard_unnecessary_functions;
    bool needsRecursionGuard(llvm::Instruction* inst);

    llvm::Value* getSingleUse(llvm::Value* v);

    // Given an Instruction with zero uses, remove that instruction,
    // and remove any instructions that now have zero uses without side effects
    void removeUnusedInsts(llvm::Instruction* inst);

    void removeCheck(const RecursionEnterCheck& enter,
                     const RecursionLeaveCheck& leave);

    bool maybeRemoveCheck(const RecursionEnterCheck& enter,
                          const RecursionLeaveCheck& leave);

public:
    static char ID;

    RemoveRecursionChecksPass() : llvm::FunctionPass(ID) {}
    ~RemoveRecursionChecksPass();

    bool runOnFunction(llvm::Function &F) override;
};

llvm::FunctionPass* createExceptionTrackingPass(nitrous::LLVMEvaluator& eval);
llvm::FunctionPass* createMiscOptsPass(nitrous::LLVMEvaluator& eval);


bool isPyObjectPtr(llvm::Type* t);
bool isPyTypeObjectPtr(llvm::Type* t);

} // namespace pystol
