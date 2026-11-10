#define _POSIX_C_SOURCE 200809L
#include "llvm_packages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int createLLVMPackages(parseResult *program, llvmPackages *units) {
    char directory[] = "/tmp/ocean-packages-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    units->directory = strdup(directory);
    units->count = program->nPackages + 1;
    units->paths = calloc(units->count, sizeof(char *));
    units->needed = calloc(units->count, 1);
    if (!units->directory || !units->paths || !units->needed) { freeLLVMPackages(units); return 1; }
    for (size_t i = 0; i < units->count; i++) {
        size_t n = strlen(directory) + 64;
        units->paths[i] = malloc(n);
        if (!units->paths[i]) { freeLLVMPackages(units); return 1; }
        if (!i) snprintf(units->paths[i], n, "%s/support.ll", directory);
        else snprintf(units->paths[i], n, "%s/package-%zu.ll", directory, i);
        units->needed[i] = 1;
    }
    return 0;
}
void freeLLVMPackages(llvmPackages *units) {
    for (size_t i = 0; units->paths && i < units->count; i++) {
        if (units->paths[i]) { unlink(units->paths[i]); free(units->paths[i]); }
    }
    free(units->paths); free(units->needed);
    if (units->directory) { rmdir(units->directory); free(units->directory); }
    memset(units, 0, sizeof(*units));
}
