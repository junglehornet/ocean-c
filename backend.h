#ifndef OCEAN_BACKEND_H
#define OCEAN_BACKEND_H
#include "parser.h"
#include <stdio.h>
/* Returns zero on success. Does not take ownership of the AST or output. */
int emitLLVM(parseResult *program, FILE *output);
/* Uses clang from PATH, with the host target and system linker. */
int compileLLVM(const char *irPath, const char *outputPath);
int compileLLVMObject(const char *irPath, const char *outputPath);
#endif
