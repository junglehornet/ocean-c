#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "backend.h"
#include "build_cache.h"
#include "llvm_packages.h"
#include "semantic.h"
#include "package.h"
#include "lsp.h"
#include "ocean_build_info.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile pid_t runningChild;
static volatile sig_atomic_t interrupted;
static void forwardSignal(int signal) {
    interrupted = signal;
    if (runningChild > 0) kill((pid_t)runningChild, signal);
}
static int writeIR(const char *input, const char *output) {
    size_t size = strlen(output) + 16;
    char *staged = malloc(size);
    if (!staged) return 1;
    snprintf(staged, size, "%s.XXXXXX", output);
    int fd = mkstemp(staged), failed = 0;
    FILE *from = fopen(input, "rb"), *to = fd < 0 ? NULL : fdopen(fd, "wb");
    if (!from || !to) { perror("could not create IR output"); failed = 1; }
    else {
        char buffer[8192]; size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), from)))
            if (fwrite(buffer, 1, n, to) != n) { failed = 1; break; }
        if (ferror(from)) failed = 1;
    }
    if (from) fclose(from);
    if (to) { if (fclose(to)) failed = 1; }
    else if (fd >= 0) close(fd);
    if (!failed && rename(staged, output)) { perror("could not save IR"); failed = 1; }
    unlink(staged); free(staged); return failed;
}
static int sameFile(const char *a, const char *b) {
#ifdef _WIN32
    char *x = realpath(a, NULL), *y = realpath(b, NULL);
    int same = x && y && !_stricmp(x, y); free(x); free(y); return same;
#else
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
#endif
}
static int nativeBuild(buildCache *cache, parseResult *program, const char *output, int library, int staticLink) {
    // Without reusable objects, lower only reachable executable functions and
    // avoid the package module serialization and separate compilation costs.
    if (!cache) {
        char path[4096]; oceanTemporary(path, sizeof(path), "ocean-ir-XXXXXX");
        int fd = mkstemp(path);
        if (fd < 0) { perror("ocean: could not create IR file"); return 1; }
        FILE *ir = fdopen(fd, "w");
        if (!ir) { perror("ocean: could not open IR file"); close(fd); unlink(path); return 1; }
        int failed = emitLLVM(program, ir);
        if (fclose(ir)) failed = 1;
        if (!failed) failed = library ? compileLLVMObject(path, output) : compileLLVMExecutable(path, output, staticLink);
        unlink(path);
        return failed;
    }
    llvmPackages units = {0};
    if (createLLVMPackages(program, &units)) { perror("native package directory"); return 1; }
    char **objects = calloc(units.count, sizeof(char *));
    char (*keys)[65] = calloc(units.count, sizeof(*keys));
    int failed = !objects || !keys, missing = 0;
    for (size_t i = 0; !failed && i < units.count; i++) {
        objects[i] = malloc(strlen(units.paths[i]) + 3);
        if (!objects[i]) { failed = 1; break; }
        sprintf(objects[i], "%s.o", units.paths[i]);
        size_t owner = i ? program->packages[i - 1].id : 0;
        int keyed = buildCachePackageKey(cache, program, owner, 0, keys[i]);
        if (keyed && readBuildCache(cache, keys[i], objects[i])) units.needed[i] = 0;
        else missing = 1;
    }
    char metadataKey[65] = {0}, *metadata = malloc(strlen(units.directory) + 16);
    if (!metadata) failed = 1;
    else sprintf(metadata, "%s/link.info", units.directory);
    int metadataHit = !failed && buildCachePackageKey(cache, program, 0, 1, metadataKey) && readBuildCache(cache, metadataKey, metadata);
    if (metadataHit) {
        FILE *file = fopen(metadata, "r");
        int value = file ? fgetc(file) : EOF;
        int threads = file ? fgetc(file) : EOF;
        int curl = file ? fgetc(file) : EOF;
        int processes = file ? fgetc(file) : EOF;
        metadataHit = file && (value == '0' || value == '1') && (threads == '0' || threads == '1') && (curl == '0' || curl == '1') && (processes == '0' || processes == '1') && fgetc(file) == EOF;
        if (file) fclose(file);
        if (metadataHit) { units.needsFFI = value == '1'; units.needsThreads = threads == '1'; units.needsCurl = curl == '1'; units.needsProcess = processes == '1'; }
    }
    if (!failed && (missing || !metadataHit)) {
        if (!metadataHit) units.needed[0] = 1;
        failed = emitLLVMPackages(program, &units);
        for (size_t i = 0; !failed && i < units.count; i++) {
            if (!units.needed[i]) continue;
            failed = compileLLVMObject(units.paths[i], objects[i]);
            char after[65]; size_t owner = i ? program->packages[i - 1].id : 0;
            if (!failed && *keys[i] && buildCachePackageKey(cache, program, owner, 0, after) && !strcmp(keys[i], after))
                storeBuildCache(cache, keys[i], objects[i]);
        }
        if (!failed && *metadataKey) {
            FILE *file = fopen(metadata, "w");
            if (file) {
                int ok = fputc(units.needsFFI ? '1' : '0', file) != EOF && fputc(units.needsThreads ? '1' : '0', file) != EOF && fputc(units.needsCurl ? '1' : '0', file) != EOF && fputc(units.needsProcess ? '1' : '0', file) != EOF;
                if (fclose(file)) ok = 0;
                if (ok) storeBuildCache(cache, metadataKey, metadata);
            }
        }
    }
    if (!failed) failed = linkLLVMObjects((const char **)objects, units.count, units.needsFFI, units.needsThreads, units.needsCurl, units.needsProcess, output, library, staticLink);
    if (metadata) { unlink(metadata); free(metadata); }
    if (objects) {
        for (size_t i = 0; i < units.count; i++)
            if (objects[i]) { unlink(objects[i]); free(objects[i]); }
        free(objects);
    }
    free(keys); freeLLVMPackages(&units); return failed;
}
static int packageMain(int argc, char **argv) {
    int run = !strcmp(argv[1], "run"), check = !strcmp(argv[1], "check"), emit = 0, staticLink = 0, argumentStart = argc, programArgumentCount = 0;
    const char *location = NULL, *output = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            argumentStart = i + 1;
            if (run) for (int j = i + 1; j < argc; j++) argv[2 + programArgumentCount++] = argv[j];
            break;
        }
        if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "ocean: -o requires an output path\n"); return 1; }
            output = argv[i];
        } else if (!strcmp(argv[i], "--emit-llvm")) emit = 1;
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--static") || !strcmp(argv[i], "-s")) staticLink = 1;
        else if (argv[i][0] == '-') {
            if (run && location) { argv[2 + programArgumentCount++] = argv[i]; continue; }
            fprintf(stderr, "ocean: unknown option: %s\n", argv[i]); return 1;
        } else if (!location) location = argv[i];
        else if (run) argv[2 + programArgumentCount++] = argv[i];
        else { fprintf(stderr, "ocean build accepts one package location\n"); return 1; }
    }
    if ((staticLink && (check || emit)) || (run && (output || emit || check)) || (check && (output || emit)) || (!run && argumentStart != argc)) {
        fprintf(stderr, "ocean: incompatible build/run options\n"); return 1;
    }
    if (!location) location = ".";
    packageBuild *build = loadPackage(location);
    parseResult *program = packageProgram(build);
    if (!program) { freePackage(build); return 1; }
    if (staticLink && program->isLibrary) {
        fprintf(stderr, "ocean: --static requires a main package executable\n");
        freePackage(build); return 1;
    }
    if (run && program->isLibrary) {
        fprintf(stderr, "ocean run: package %s is not main\n", program->packageName);
        freePackage(build); return 1;
    }
    int failed = validateBackendTarget() || analyzeSemantics(program).nErrors != 0;
    if (!failed && run && backendWasm()) {
        fprintf(stderr, "ocean run: WebAssembly output must be run by a WebAssembly host\n");
        freePackage(build); return 1;
    }
    if (check && !failed) failed = checkBackend(program);
    if (check && !failed) {
        packageBuild *tests = loadPackageTests(location);
        if (tests) {
            parseResult *testProgram = packageProgram(tests);
            failed = !testProgram || analyzeSemantics(testProgram).nErrors != 0;
            if (!failed) failed = checkBackend(testProgram);
            freePackage(tests);
        }
    }
    if (failed || check) { freePackage(build); return failed; }
    char *temporaryDirectory = NULL, *temporaryExecutable = NULL, *defaultOutput = NULL;
    struct sigaction oldInterrupt, oldTerminate, oldHangup;
    if (run) {
        const char *base = getenv("TMPDIR");
#ifdef _WIN32
        if (!base) base = getenv("TEMP");
#endif
        if (!base) base = "/tmp";
        temporaryDirectory = malloc(strlen(base) + 32);
        if (!temporaryDirectory) { freePackage(build); return 1; }
        sprintf(temporaryDirectory, "%s/.ocean-run-XXXXXX", base);
        if (!mkdtemp(temporaryDirectory)) {
            perror("ocean run: temporary directory"); free(temporaryDirectory); freePackage(build); return 1;
        }
        temporaryExecutable = malloc(strlen(temporaryDirectory) + 16);
        if (!temporaryExecutable) { rmdir(temporaryDirectory); free(temporaryDirectory); freePackage(build); return 1; }
        sprintf(temporaryExecutable, "%s/program" OCEAN_EXECUTABLE_SUFFIX, temporaryDirectory);
        output = temporaryExecutable;
        struct sigaction action = {0}; action.sa_handler = forwardSignal;
        sigemptyset(&action.sa_mask);
        interrupted = runningChild = 0;
        sigaction(SIGINT, &action, &oldInterrupt);
        sigaction(SIGTERM, &action, &oldTerminate);
        sigaction(SIGHUP, &action, &oldHangup);
    } else if (!output) {
        // The directory's package name determines library artifacts; main builds
        // use the directory name, matching the package-oriented CLI.
        if (emit) output = "a.ll";
        else {
            const char *base = program->packageName;
            if (!program->isLibrary) {
                char *path = strdup(packageDirectory(build));
                char *slash = strrchr(path, '/');
                base = slash ? slash + 1 : path;
                defaultOutput = strdup(*base ? base : "main");
                free(path);
            } else {
                defaultOutput = malloc(strlen(base) + 3);
                if (defaultOutput) sprintf(defaultOutput, "%s.o", base);
            }
            output = defaultOutput;
        }
    }
    if (!output) failed = 1;
    for (size_t i = 0; !failed && i < program->nSources; i++)
        if (sameFile(output, program->sources[i].filename)) {
            fprintf(stderr, "ocean: output would overwrite an input file\n"); failed = 1;
        }
    buildCache *cache = failed ? NULL : openBuildCache(argv[0], program, packageDirectory(build));
    char irPath[4096]; oceanTemporary(irPath, sizeof(irPath), "ocean-ir-XXXXXX");
    int fd = failed || !emit ? -1 : mkstemp(irPath);
    if (!failed && emit && fd < 0) { perror("ocean: could not create IR file"); failed = 1; }
    if (fd >= 0) close(fd);
    if (!failed && emit && !readBuildCache(cache, buildCacheIRKey(cache), irPath)) {
        FILE *ir = fopen(irPath, "w");
        if (!ir) { perror("ocean: could not create IR file"); failed = 1; }
        else {
            failed = emitLLVM(program, ir);
            if (fclose(ir)) failed = 1;
            if (!failed) storeBuildCache(cache, buildCacheIRKey(cache), irPath);
        }
    }
    if (!failed) failed = emit ? writeIR(irPath, output) : nativeBuild(cache, program, output, program->isLibrary, staticLink);
    if (emit) unlink(irPath);
    closeBuildCache(cache);
    freePackage(build);
    int status = failed ? 1 : 0;
    if (run && !failed && !interrupted) {
        char **arguments = calloc((size_t)programArgumentCount + 2, sizeof(*arguments));
        if (!arguments) status = 1;
        else {
            arguments[0] = temporaryExecutable;
            for (int i = 0; i < programArgumentCount; i++) arguments[i + 1] = argv[i + 2];
            sigset_t blocked, previous;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGHUP);
            sigprocmask(SIG_BLOCK, &blocked, &previous);
            pid_t child = oceanSpawn(arguments, NULL, 0);
            if (child < 0) { sigprocmask(SIG_SETMASK, &previous, NULL); perror("ocean run: fork"); status = 1; }
            else {
                runningChild = child;
                sigprocmask(SIG_SETMASK, &previous, NULL);
                if (interrupted) kill(child, interrupted);
                int childStatus;
                while (waitpid(child, &childStatus, 0) < 0) {
                    if (errno == EINTR) continue;
                    perror("ocean run: waitpid");
                    childStatus = -1; break;
                }
                runningChild = 0;
                if (childStatus != -1 && WIFSIGNALED(childStatus)) {
                    int signalNumber = WTERMSIG(childStatus);
                    fprintf(stderr, "ocean run: program terminated by signal %d (%s)\n",
                            signalNumber, strsignal(signalNumber));
                }
                status = childStatus == -1 ? 1 : WIFEXITED(childStatus) ? WEXITSTATUS(childStatus)
                                                                  : 128 + WTERMSIG(childStatus);
            }
            free(arguments);
        }
    } else if (run && interrupted) status = 128 + interrupted;
    if (run) {
        unlink(temporaryExecutable); rmdir(temporaryDirectory);
        sigaction(SIGINT, &oldInterrupt, NULL); sigaction(SIGTERM, &oldTerminate, NULL); sigaction(SIGHUP, &oldHangup, NULL);
    }
    free(temporaryExecutable); free(temporaryDirectory); free(defaultOutput);
    return status;
}
#include "test_runner.inc"

static int moduleMain(int argc, char **argv) {
    const char *usage = "Usage: ocean mod init [name]\n";
    if ((argc == 3 && !strcmp(argv[2], "--help")) ||
        (argc == 4 && !strcmp(argv[2], "init") && !strcmp(argv[3], "--help"))) {
        fputs(usage, stdout); return 0;
    }
    if (argc < 3 || argc > 4 || strcmp(argv[2], "init")) {
        fputs(usage, stderr); return 1;
    }
    char *currentDirectory = NULL;
    const char *name = argc == 4 ? argv[3] : NULL;
    if (!name) {
        currentDirectory = getcwd(NULL, 0);
        if (!currentDirectory) { perror("ocean mod init: current directory"); return 1; }
        const char *slash = strrchr(currentDirectory, '/');
        name = slash ? slash + 1 : currentDirectory;
    }
    if (!validModulePath(name)) {
        fprintf(stderr, "ocean mod init: invalid module name; provide a name such as github.com/user/project\n");
        free(currentDirectory); return 1;
    }
    int fd = open("ocean.mod", O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) fprintf(stderr, "ocean mod init: ocean.mod already exists\n");
        else perror("ocean mod init: cannot create ocean.mod");
        free(currentDirectory); return 1;
    }
    FILE *file = fdopen(fd, "w");
    int failed = !file;
    if (file) {
        if (fprintf(file, "module %s\n\nocean 0.1\n", name) < 0) failed = 1;
        if (fclose(file)) failed = 1;
    } else close(fd);
    if (failed) {
        perror("ocean mod init: cannot write ocean.mod"); unlink("ocean.mod");
    } else printf("ocean: created ocean.mod for module %s\n", name);
    free(currentDirectory);
    return failed;
}
int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY); _setmode(_fileno(stderr), _O_BINARY);
    _setmaxstdio(2048);
    if (argc == 4 && !strcmp(argv[1], "--internal-lsp-worker")) return oceanLspWorker(argv[2], argv[3]);
    if (argc > 1 && !strcmp(argv[1], "--internal-test-package")) { sharedBudget = 1; argv[1] = "test"; return testOneMain(argc, argv); }
#endif
    if (argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
        printf("Ocean compiler (ocean-c) %s\nCommit %s (%s), built %s\nCopyright Connor Egan %s\n",
               OCEAN_VERSION, OCEAN_COMMIT, OCEAN_COMMIT_DATE, OCEAN_BUILD_DATE, OCEAN_COMMIT_YEAR);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "test")) return testMain(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "check")) {
        if (argc == 3 && !strcmp(argv[2], "--lsp")) return oceanLsp();
        if (argc == 3 && !strcmp(argv[2], "--help")) {
            puts("Usage: ocean check [location]\n       ocean check --lsp"); return 0;
        }
        return packageMain(argc, argv);
    }
    if (argc > 1 && !strcmp(argv[1], "clean")) {
        if (argc == 3 && (!strcmp(argv[2], "--cache") || !strcmp(argv[2], "-cache"))) return cleanBuildCache();
        if (argc == 3 && !strcmp(argv[2], "--help")) { puts("Usage: ocean clean --cache"); return 0; }
        fputs("Usage: ocean clean --cache\n", stderr); return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "mod")) return moduleMain(argc, argv);
    if (argc > 1 && (!strcmp(argv[1], "build") || !strcmp(argv[1], "run"))) {
        if (argc == 3 && !strcmp(argv[2], "--help")) {
            printf("Usage: ocean build [--check | --emit-llvm | --static | -s] [-o output] [location]\n"
                   "       ocean run [--static | -s] [location] [program-arguments...] [-- program-arguments...]\n");
            return 0;
        }
        return packageMain(argc, argv);
    }
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")))) {
        printf("Usage: ocean build [--check | --emit-llvm | --static | -s] [-o output] [location]\n"
               "       ocean run [--static | -s] [location] [program-arguments...] [-- program-arguments...]\n"
               "       ocean test [location] [--json] [--outfile path] [--jsonfile path]\n"
               "       ocean check [location]\n"
               "       ocean check --lsp\n"
               "       ocean mod init [name]\n"
               "       ocean clean --cache\n"
               "       ocean -v | --version\n");
        return argc == 1 ? 1 : 0;
    }
    fprintf(stderr, "ocean: unknown command: %s\n", argv[1]);
    return 1;
}
