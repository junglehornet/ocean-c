#ifndef OCEAN_TEST_SUPPORT_H
#define OCEAN_TEST_SUPPORT_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

// Isolate APIs that exit/abort, and inspect their actual diagnostics. A file
// avoids pipe-buffer deadlocks when a child prints more than a pipe can hold.
static char *capture_child(void (*action)(void), int *status) {
    fflush(NULL);
    FILE *output = tmpfile();
    assert(output);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit no_core = {0, 0};
        assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
        assert(dup2(fileno(output), STDOUT_FILENO) >= 0);
        assert(dup2(fileno(output), STDERR_FILENO) >= 0);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
#ifdef OCEAN_TEST_COVERAGE
        extern void __llvm_profile_set_filename(const char *);
        char profile[4096];
        const char *profile_dir = getenv("OCEAN_PROFILE_DIR");
        assert(profile_dir);
        assert(snprintf(profile, sizeof(profile), "%s/child-%ld.profraw",
                        profile_dir, (long)getpid()) < (int)sizeof(profile));
        __llvm_profile_set_filename(profile);
#endif
        action();
        fflush(NULL);
#ifdef OCEAN_TEST_COVERAGE
        extern int __llvm_profile_write_file(void);
        __llvm_profile_write_file();
#endif
        _Exit(0);
    }
    assert(waitpid(child, status, 0) == child);
    assert(fseek(output, 0, SEEK_END) == 0);
    long size = ftell(output);
    assert(size >= 0 && fseek(output, 0, SEEK_SET) == 0);
    char *text = calloc((size_t)size + 1, 1);
    assert(text && fread(text, 1, (size_t)size, output) == (size_t)size);
    fclose(output);
    return text;
}
#endif
