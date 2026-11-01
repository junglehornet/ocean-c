#ifndef OCEAN_LLVM_PACKAGES_H
#define OCEAN_LLVM_PACKAGES_H
#include "parser.h"
typedef struct {
    size_t count;
    char **paths;
    char *directory;
} llvmPackages;
int splitLLVMPackages(parseResult *program, const char *path, llvmPackages *units);
void freeLLVMPackages(llvmPackages *units);
#endif
