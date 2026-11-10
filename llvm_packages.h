#ifndef OCEAN_LLVM_PACKAGES_H
#define OCEAN_LLVM_PACKAGES_H
#include "parser.h"
/* Slot zero owns shared globals/runtime/specializations. Other slots are source
 * packages in dependency order. Cache hits can leave their IR paths unwritten. */
typedef struct {
    size_t count;
    char **paths;
    char *directory;
    unsigned char *needed;
    int needsFFI;
} llvmPackages;
int createLLVMPackages(parseResult *program, llvmPackages *units);
void freeLLVMPackages(llvmPackages *units);
#endif
