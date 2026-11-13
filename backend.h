#ifndef OCEAN_BACKEND_H
#define OCEAN_BACKEND_H
#include "parser.h"
#include "llvm_packages.h"
#include <stdio.h>
/* Returns zero on success. Does not take ownership of the AST or output. */
int backendWasm(void);
int validateBackendTarget(void);
int emitLLVM(parseResult *program, FILE *output);
/* Direct emission; no merged LLVM file is parsed or split. */
int emitLLVMPackages(parseResult *program, llvmPackages *units);
/* Validate lowering, LLVM verification, and the normal link contract without publishing output. */
int checkBackend(parseResult *program);
int linkLLVMObjects(const char **objects, size_t count, int needsFFI, int needsThreads, int needsCurl, int needsProcess, const char *output, int library, int staticLink);
/* Uses clang from PATH; OCOS=js OCARCH=wasm selects wasm32, otherwise host. */
int compileLLVMExecutable(const char *irPath, const char *outputPath, int staticLink);
int compileLLVM(const char *irPath, const char *outputPath);
int compileLLVMObject(const char *irPath, const char *outputPath);
/* Always relink cached objects against the current system libraries. */
int linkLLVMObject(const char *objectPath, const char *irPath, const char *outputPath);
#endif
