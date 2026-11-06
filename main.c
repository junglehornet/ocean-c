#define _POSIX_C_SOURCE 200809L
#include "backend.h"
#include "build_cache.h"
#include "llvm_packages.h"
#include "semantic.h"
#include "package.h"
#include "lsp.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t runningChild, interrupted;
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
    struct stat x, y;
    return !stat(a, &x) && !stat(b, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}
static int nativeBuild(buildCache *cache, parseResult *program, const char *irPath, const char *output, int library) {
    char key[65];
    if (!buildCacheObjectKey(cache,irPath,key)) return library?compileLLVMObject(irPath,output):compileLLVM(irPath,output);
    llvmPackages units={0};
    if(splitLLVMPackages(program,irPath,&units))return 1;
    char **objects=calloc(units.count,sizeof(char*));int failed=!objects;
    for(size_t i=0;!failed&&i<units.count;i++){
        objects[i]=malloc(strlen(units.paths[i])+3);if(!objects[i]){failed=1;break;}
        sprintf(objects[i],"%s.o",units.paths[i]);
        if(!buildCacheObjectKey(cache,units.paths[i],key)){failed=1;break;}
        if(!readBuildCache(cache,key,objects[i])){
            failed=compileLLVMObject(units.paths[i],objects[i]);char after[65];
            if(!failed&&buildCacheObjectKey(cache,units.paths[i],after)&&!strcmp(key,after))storeBuildCache(cache,key,objects[i]);
        }
    }
    if(!failed)failed=linkLLVMObjects((const char**)objects,units.count,irPath,output,library);
    if(objects){for(size_t i=0;i<units.count;i++){if(objects[i]){unlink(objects[i]);free(objects[i]);}}free(objects);}
    freeLLVMPackages(&units);return failed;
}
static int packageMain(int argc, char **argv) {
    int run = !strcmp(argv[1], "run"), check = !strcmp(argv[1], "check"), emit = 0, argumentStart = argc, programArgumentCount = 0;
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
        else if (argv[i][0] == '-') {
            if (run && location) { argv[2 + programArgumentCount++] = argv[i]; continue; }
            fprintf(stderr, "ocean: unknown option: %s\n", argv[i]); return 1;
        } else if (!location) location = argv[i];
        else if (run) argv[2 + programArgumentCount++] = argv[i];
        else { fprintf(stderr, "ocean build accepts one package location\n"); return 1; }
    }
    if ((run && (output || emit || check)) || (check && (output || emit)) || (!run && argumentStart != argc)) {
        fprintf(stderr, "ocean: incompatible build/run options\n"); return 1;
    }
    if (!location) location = ".";
    packageBuild *build = loadPackage(location);
    parseResult *program = packageProgram(build);
    if (!program) { freePackage(build); return 1; }
    if (run && program->isLibrary) {
        fprintf(stderr, "ocean run: package %s is not main\n", program->packageName);
        freePackage(build); return 1;
    }
    int failed = analyzeSemantics(program).nErrors != 0;
    if (check && !failed) failed = checkBackend(program);
    if (failed || check) { freePackage(build); return failed; }
    char *temporaryDirectory = NULL, *temporaryExecutable = NULL, *defaultOutput = NULL;
    struct sigaction oldInterrupt, oldTerminate, oldHangup;
    if (run) {
        const char *base = getenv("TMPDIR");
        if (!base) base = "/tmp";
        temporaryDirectory = malloc(strlen(base) + 32);
        if (!temporaryDirectory) { freePackage(build); return 1; }
        sprintf(temporaryDirectory, "%s/.ocean-run-XXXXXX", base);
        if (!mkdtemp(temporaryDirectory)) {
            perror("ocean run: temporary directory"); free(temporaryDirectory); freePackage(build); return 1;
        }
        temporaryExecutable = malloc(strlen(temporaryDirectory) + 16);
        if (!temporaryExecutable) { rmdir(temporaryDirectory); free(temporaryDirectory); freePackage(build); return 1; }
        sprintf(temporaryExecutable, "%s/program", temporaryDirectory);
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
    char irPath[] = "/tmp/ocean-ir-XXXXXX";
    int fd = failed ? -1 : mkstemp(irPath);
    if (!failed && fd < 0) { perror("ocean: could not create IR file"); failed = 1; }
    if (fd >= 0) close(fd);
    if (!failed && !readBuildCache(cache, buildCacheIRKey(cache), irPath)) {
        FILE *ir = fopen(irPath, "w");
        if (!ir) { perror("ocean: could not create IR file"); failed = 1; }
        else {
            failed = emitLLVM(program, ir);
            if (fclose(ir)) failed = 1;
            if (!failed) storeBuildCache(cache, buildCacheIRKey(cache), irPath);
        }
    }
    if (!failed) failed = emit ? writeIR(irPath, output) : nativeBuild(cache, program, irPath, output, program->isLibrary);
    unlink(irPath); closeBuildCache(cache);
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
            pid_t child = fork();
            if (!child) {
                signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
                sigprocmask(SIG_SETMASK, &previous, NULL);
                execv(temporaryExecutable, arguments); perror("ocean run"); _exit(127);
            }
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
            printf("Usage: ocean build [--check | --emit-llvm] [-o output] [location]\n"
                   "       ocean run [location] [program-arguments...] [-- program-arguments...]\n");
            return 0;
        }
        return packageMain(argc, argv);
    }
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help")))) {
        printf("Usage: ocean build [--check | --emit-llvm] [-o output] [location]\n"
               "       ocean run [location] [program-arguments...] [-- program-arguments...]\n"
               "       ocean check [location]\n"
               "       ocean check --lsp\n"
               "       ocean mod init [name]\n"
               "       ocean clean --cache\n");
        return argc == 1 ? 1 : 0;
    }
    fprintf(stderr, "ocean: unknown command: %s\n", argv[1]);
    return 1;
}
