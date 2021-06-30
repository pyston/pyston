#ifndef _NITROUS_OPTIMIZATIONHOOKS_H
#define _NITROUS_OPTIMIZATIONHOOKS_H

#include <memory>
#include <unordered_map>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/GenericValue.h"

namespace llvm {
class DataLayout;
class LLVMContext;
class Function;
class FunctionPass;
class Instruction;
class Value;
}

namespace nitrous {

llvm::LLVMContext& getContext();
const llvm::DataLayout* getDataLayout();

void verbosePrint(llvm::Function*);

class LLVMEvaluator {
public:
    LLVMEvaluator() = default;
    virtual ~LLVMEvaluator() = default;
    virtual llvm::GenericValue loadPointer(void* ptr, llvm::Type* type) = 0;
    virtual llvm::GenericValue evalConstant(const llvm::Constant* val) = 0;
    virtual llvm::Constant* GVToConst(const llvm::GenericValue& gv, llvm::Type* type) = 0;
};

#define JIT_IS_CONST    (1<<0)
#define JIT_NOT_ZERO    (1<<1)
typedef struct _JitConst {
    char* addr;
    int size;
    int flags;
} JitConst;

void addMallocLikeFunc(const char* name);
void addJitConst(char* addr, int size, int flags);
void addJitConsts(JitConst* consts, int num_consts);

// If you change this, make sure to update facts.cpp:unionInto and intersectInto
struct Knowledge {
    bool isnonzero = 0, isheapalloc = 0;

    llvm::Value* known_value = nullptr;
    // known_at specifies that we know a specific load resolved to the value known_value
    // known_at == NULL means it always loads to that value (subject to domain constraints)
    llvm::Instruction* known_at = nullptr;

    void dump() const;
};

// The Location struct specifies which part of a Value we have information about.
// If the Location is empty (empty indirections array), the knowledge is about the
// value of the Value directly.
// Otherwise each indirection specifies a byte offset+size, which are done from first to last.
struct Location {
    struct Indirection {
        int offset;
        int size;

        Indirection(int offset, int size) : offset(offset), size(size) {}

        bool operator==(const Indirection rhs) const {
            return offset == rhs.offset && size == rhs.size;
        }
    };

    llvm::SmallVector<Indirection, 1> indirections;

    Location() {}

    Location(int offset, int size) {
        indirections.push_back(Indirection{offset, size});
    }

    Location(std::pair<int, int> loc) {
        indirections.push_back(Indirection{loc.first, loc.second});
    }

    Location(llvm::SmallVector<Indirection, 1> indirections)
        : indirections(indirections) {}

    // Returns a Location that is a gep of the current Location with the given byte offset
    Location gepDest(int offset, int size) const;

    // Returns the Location that would produce this Location if the specified
    // gep offset was applied to it
    Location gepSource(int offset, int size) const {
        return gepDest(-offset, size);
    }

    void dump() const;

    bool operator==(const Location& rhs) const {
        return indirections == rhs.indirections;
    }
};

// Fact domains are specified by the first instruction where they are true, and consist
// of all instructions dominated by that instruction.
// If the instruction is NULL, the domain is the entire function.
struct Domain {
    llvm::Instruction* from_where;

    Domain(llvm::Instruction* from_where) : from_where(from_where) {}

    bool operator==(const Domain& rhs) const {
        return from_where == rhs.from_where;
    }
};

} // namespace nitrous

namespace std {

template <>
struct hash<nitrous::Location>
{
    size_t operator()(const nitrous::Location& l) const noexcept {
        size_t x = 0;
        for (auto&& ind : l.indirections) {
            x *= 1027;
            x += ind.offset + 107 * ind.size;
        }
        return x;
    }
};

template <>
struct hash<nitrous::Domain>
{
    size_t operator()(const nitrous::Domain& d) const noexcept {
        return hash<llvm::Instruction*>()(d.from_where);
    }
};

} // namespace std

namespace nitrous {

// A set of facts about a Value within a single domain
struct FactSet {
    std::unordered_map<Location, Knowledge> facts;

    Knowledge& operator[](const Location& location) {
        return facts[location];
    }

    void dump() const;
};

// Often we have facts that are only true in certain parts of the program.  For example facts
// may be derived from Instructions, but are then only true if that Instruction has been executed.
// FactDomains keeps track of the different domains that we have facts about, along with
// the facts in that domain.
struct FactDomains {
    std::unordered_map<Domain, FactSet> domains;

    FactSet& operator[](const Domain& domain) {
        return domains[domain];
    }

    void dump() const;
};

class FactDeriver {
public:
    virtual ~FactDeriver() {}

    // Nitrous will call this function to derive application-specific facts about a value.
    // This function should return whether the FactSet was updated.
    virtual bool deriveFacts(llvm::Value* v, FactSet& facts, LLVMEvaluator& eval) = 0;
};

void registerFactDeriver(std::unique_ptr<FactDeriver> deriver);

void registerPassFactory(std::function<llvm::FunctionPass*(LLVMEvaluator& eval)> factory);

}

#endif
