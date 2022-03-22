#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <vector>
#include <unistd.h>

#include "common.h"

#include "symbol_finder.h"

using namespace llvm;
using namespace std;

namespace nitrous {

static vector<char> getProcessOutput(const char* file, const char* arg) {
    // From
    // http://www.microhowto.info/howto/capture_the_output_of_a_child_process_in_c.html
    int filedes[2];
    if (pipe(filedes) == -1) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(1);
    } else if (pid == 0) {
        while ((dup2(filedes[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {
        }
        while ((dup2(filedes[1], STDERR_FILENO) == -1) && (errno == EINTR)) {
        }
        close(filedes[1]);
        close(filedes[0]);
        char* v[]= {(char*)file, (char*)arg, NULL};
        char* envp[] = {NULL, NULL};
        execvpe(file, v, envp);
        perror("execl");
        _exit(1);
    }
    close(filedes[1]);

    vector<char> output;

    char buffer[4096];
    while (1) {
        ssize_t count = read(filedes[0], buffer, sizeof(buffer));
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            } else {
                perror("read");
                exit(1);
            }
        } else if (count == 0) {
            break;
        } else {
            output.insert(output.end(), &buffer[0], &buffer[count]);
        }
    }
    close(filedes[0]);
    wait(0);

    return output;
}

StringMap<intptr_t> SymbolFinder::getOffsetsFromFile(llvm::StringRef filename) {
    vector<char> data = getProcessOutput("nm", filename.str().c_str());
    stringstream parser(string(&data[0], data.size()));

    StringMap<intptr_t> addresses;
    while (!parser.eof()) {
        string offset, type, name;
        parser >> offset >> type;

        if (offset == "nm:") {
            parser >> name;
            //RELEASE_ASSERT(name == "no", "");
            parser >> name;
            //RELEASE_ASSERT(name == "symbols", "");
            break;
        }

        if (offset.size() == 0) {
            // eof
            break;
        }

        if (offset.size() == 1) {
            // missing offset field
            continue;
        }
        parser >> name;

        RELEASE_ASSERT(type.size() == 1, "");

        intptr_t offset_int = strtoll(offset.c_str(), nullptr, 16);
        if (addresses.count(name))
            addresses[name] = MULTIPLY_DEFINED;
        else
            addresses[name] = offset_int;
    }
    return addresses;
}

void SymbolFinder::addSymbolsFromFile(StringRef filename,
                                      intptr_t addr_offset) {
    auto offsets = getOffsetsFromFile(filename);

    auto allowed_duplicate = [](llvm::StringRef name) {
        if (name == "__clear_cache")
            return 1;
        return 0;
    };

    for (auto& p : offsets) {
        void* addr = (void*)(p.second + addr_offset);

        if (p.second == MULTIPLY_DEFINED
            || (symbol_addresses.count(p.first()) && !allowed_duplicate(p.first())))
            symbol_addresses[p.first()] = (void*)MULTIPLY_DEFINED;
        else
            symbol_addresses[p.first()] = addr;

        // TODO: could still add these multiply-defined symbols to the reverse map
        if (p.second != MULTIPLY_DEFINED) {
            if (address_symbols.count(addr))
                address_symbols[addr] = MULTIPLY_DEFINED_STR;
            else
                address_symbols[addr] = p.first().str();
        }
    }
}

void* SymbolFinder::lookupSymbol(StringRef name) const {
    // Some symbols like 'printf' are easiest to find this way:
    if (void* from_dl = dlsym(dl_handle, name.str().c_str()))
        return from_dl;

    auto it = symbol_addresses.find(name);
    RELEASE_ASSERT(it != symbol_addresses.end(), "symbol '%s' not found",
                   name.str().c_str());

    void* addr = it->second;
    RELEASE_ASSERT(addr != (void*)MULTIPLY_DEFINED,
                   "symbol '%s' defined multiple times", name.str().c_str());

    return addr;
}

string SymbolFinder::lookupAddress(void* address) const {
    // Some symbols like "labs" are easiest to find this way:
    Dl_info info;
    int r = dladdr(address, &info);
    //RELEASE_ASSERT(r != 0, "%p", address);
    if (r != 0 && info.dli_sname)
        return info.dli_sname;

    auto it = address_symbols.find(address);

    if (it == address_symbols.end())
        return "";

    return it->second;
}

void SymbolFinder::loadMemoryMap() {
    RELEASE_ASSERT(dso_offsets.empty(), "memory map already loaded!");

    ifstream input("/proc/self/maps");

    string main_executable;

    while (true) {
        if (input.eof())
            break;

        string line;
        getline(input, line);

        stringstream line_input(line);
        string range, perms, _offset, device, inode, file;
        line_input >> range >> perms >> _offset >> device >> inode >> file;

        if (perms == "r--s")
            continue;

        /*
         * Typically the executable section comes first, so the load address
         * is the address of the executable section.
         * On some machines the executable section is not first, and it looks like
         * the relocation offset is the base address of the first section.
         * It might be more flexible than that, we might technically have to
         * read the elf file to see where the section markers are, but this
         * seems to work for now.
         */
        if (!file.size() || file[0] == '[' || dso_offsets.count(file))
            continue;

        intptr_t offset = strtoll(range.c_str(), nullptr, 16);

        if (file.find(".so") == string::npos && main_executable.empty()) {
            RELEASE_ASSERT(
                main_executable.empty(),
                "found two main executables (non-.so's): '%s' and '%s'",
                main_executable.c_str(), file.c_str());
            main_executable = file;

            // HACK
            offset = 0;
        }

        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
            printf("symb load 0x%lx %s\n", offset, file.c_str());

        dso_offsets[file] = offset;
    }
    fflush(stdout);

    for (auto& p : dso_offsets) {
        addSymbolsFromFile(p.first(), p.second);
    }
}

SymbolFinder::SymbolFinder() {
    loadMemoryMap();

    dl_handle = dlopen(nullptr, RTLD_NOW);
}
}
