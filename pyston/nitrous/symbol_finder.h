#ifndef _NITROUS_SYMBOLFINDER_H
#define _NITROUS_SYMBOLFINDER_H

#include <string>
#include <unordered_map>

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"

namespace nitrous {

class SymbolFinder {
private:
    static constexpr intptr_t MULTIPLY_DEFINED = -1;
    static constexpr const char* MULTIPLY_DEFINED_STR = " MULTIPLY_DEFINED";
    llvm::StringMap<void*> symbol_addresses;
    std::unordered_map<void*, std::string> address_symbols;

    llvm::StringMap<intptr_t> dso_offsets;

    llvm::StringMap<intptr_t> getOffsetsFromFile(llvm::StringRef filename);
    void addSymbolsFromFile(llvm::StringRef filename, intptr_t addr_offset);

    void loadMemoryMap();

    void* dl_handle;

public:
    SymbolFinder();

    void* lookupSymbol(llvm::StringRef name) const;
    std::string lookupAddress(void* address) const;
};

}

#endif
