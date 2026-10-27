#define _POSIX_C_SOURCE 200809L
#include "backend.h"
#include "dynarray.h"
#include "parser.h"
#include "scanner.h"
#include "semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int checkOnly = 0, emitOnly = 0;
    const char *output = NULL;
    char **inputs = calloc((size_t)argc, sizeof(*inputs));
    if (!inputs)
        return 1;
    size_t nInputs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check"))
            checkOnly = 1;
        else if (!strcmp(argv[i], "--emit-llvm"))
            emitOnly = 1;
        else if (!strcmp(argv[i], "-o")) {
            if (++i == argc) {
                fprintf(stderr, "Error: -o requires an output path\n");
                free(inputs);
                return 1;
            }
            output = argv[i];
        } else if (!strcmp(argv[i], "--help")) {
            printf("Usage: ocean-c [--check | --emit-llvm] [-o output] source.oc\n");
            free(inputs);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option: %s\n", argv[i]);
            free(inputs);
            return 1;
        } else
            inputs[nInputs++] = argv[i];
    }
    if (!nInputs) {
        fprintf(stderr, "Error: no input files included.\n");
        free(inputs);
        return 1;
    }
    if ((checkOnly && (emitOnly || output)) || (!checkOnly && nInputs != 1)) {
        fprintf(stderr, "Error: compilation requires one input file; --check cannot be combined "
                        "with output options\n");
        free(inputs);
        return 1;
    }
    if (!output)
        output = emitOnly ? "a.ll" : "a.out";
    if (!checkOnly) {
        struct stat inStat, outStat;
        if (!stat(inputs[0], &inStat) && !stat(output, &outStat) &&
            inStat.st_dev == outStat.st_dev && inStat.st_ino == outStat.st_ino) {
            fprintf(stderr, "Error: output would overwrite the input file\n");
            free(inputs);
            return 1;
        }
    }
    for (size_t i = 0; i < nInputs; i++) {
        FILE *sourcefile = fopen(inputs[i], "rb");
        if (!sourcefile) {
            printf("Error: file not found: %s\n", inputs[i]);
            exit(1);
        }

        struct stat sourceStat;
        if (fstat(fileno(sourcefile), &sourceStat) != 0 ||
            !S_ISREG(sourceStat.st_mode) || fseek(sourcefile, 0, SEEK_END) != 0) {
            printf("Error: could not read input file: %s\n", inputs[i]);
            fclose(sourcefile);
            return 1;
        }
        long length = ftell(sourcefile);
        if (length < 0 || fseek(sourcefile, 0, SEEK_SET) != 0) {
            printf("Error: could not read input file: %s\n", inputs[i]);
            fclose(sourcefile);
            return 1;
        }
        size_t fsize = (size_t)length;

        char *source = malloc(fsize + 1);
        if (!source) {
            printf("Error: Out of memory - could not malloc source code buffer\n");
            exit(1);
        }
        if (fread(source, 1, fsize, sourcefile) != fsize || ferror(sourcefile)) {
            printf("Error: could not read input file: %s\n", inputs[i]);
            free(source);
            fclose(sourcefile);
            return 1;
        }
        fclose(sourcefile);
        source[fsize] = 0;

        tokenlist *list = scanTokens(source, inputs[i]);
        if (checkOnly) {
            printTokensFromFile(inputs[i], list);
            printf("\n\n");
        }
        parseResult res = parseTokens(list);
        res.sourceText = source;
        res.sourceFilename = inputs[i];
        semanticResult semantics = {0};
        if (!res.hasError)
            semantics = analyzeSemantics(&res);
        int status = res.hasError || semantics.nErrors;
        if (!status && !checkOnly) {
            char irPath[] = "/tmp/ocean-ir-XXXXXX";
            int fd = mkstemp(irPath);
            FILE *ir = fd < 0 ? NULL : fdopen(fd, "w");
            if (!ir) {
                perror("could not create LLVM IR file");
                if (fd >= 0)
                    close(fd);
                status = 1;
            } else {
                status = emitLLVM(&res, ir);
                if (fclose(ir))
                    status = 1;
                if (!status && emitOnly) {
                    FILE *from = fopen(irPath, "rb"), *to = NULL;
                    size_t len = strlen(output) + 16;
                    char *staged = malloc(len);
                    if (staged) {
                        snprintf(staged, len, "%s.XXXXXX", output);
                        int outFd = mkstemp(staged);
                        if (outFd >= 0) {
                            to = fdopen(outFd, "wb");
                            if (!to)
                                close(outFd);
                        }
                    }
                    if (!from || !to) {
                        perror("could not create IR output");
                        status = 1;
                    } else {
                        char buffer[4096];
                        size_t n;
                        while ((n = fread(buffer, 1, sizeof(buffer), from))) {
                            if (fwrite(buffer, 1, n, to) != n) {
                                status = 1;
                                break;
                            }
                        }
                        if (ferror(from))
                            status = 1;
                    }
                    if (from)
                        fclose(from);
                    if (to && fclose(to))
                        status = 1;
                    if (!status && rename(staged, output)) {
                        perror("could not save LLVM IR");
                        status = 1;
                    }
                    if (staged) {
                        unlink(staged);
                        free(staged);
                    }
                } else if (!status)
                    status = compileLLVM(irPath, output);
            }
            unlink(irPath);
        }
        free(list);
        free(source);
        if (status) {
            free(inputs);
            return 1;
        }
    }
    free(inputs);
    return 0;
}
