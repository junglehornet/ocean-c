#ifndef OCEAN_BACKEND_H
#define OCEAN_BACKEND_H
#include "parser.h"
#include <stdio.h>
/* Returns zero on success. Does not take ownership of the AST or output. */
int emitLLVM(parseResult *program, FILE *output);
/* Validate lowering, LLVM verification, and the normal link contract without publishing output. */
int checkBackend(parseResult *program);
int linkLLVMObjects(const char **objects, size_t count, const char *irPath, const char *output, int library);
/* Uses clang from PATH, with the host target and system linker. */
int compileLLVM(const char *irPath, const char *outputPath);
int compileLLVMObject(const char *irPath, const char *outputPath);
/* Always relink cached objects against the current system libraries. */
int linkLLVMObject(const char *objectPath, const char *irPath, const char *outputPath);
#endif
