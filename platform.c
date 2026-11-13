#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#ifndef _WIN32
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
pid_t oceanSpawn(char *const argv[], const oceanDescriptor *descriptors, size_t count) {
    pid_t child = fork();
    if (!child) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        sigset_t signals; sigemptyset(&signals); sigprocmask(SIG_SETMASK, &signals, NULL);
        for (size_t i = 0; i < count; i++)
            if (dup2(descriptors[i].source, descriptors[i].destination) < 0) _exit(127);
        execvp(argv[0], argv); perror(argv[0]); _exit(127);
    }
    return child;
}
char *oceanExecutable(void) {
#ifdef __APPLE__
    uint32_t size = 0; _NSGetExecutablePath(NULL, &size);
    char *path = malloc(size);
    if (!path || _NSGetExecutablePath(path, &size)) { free(path); return NULL; }
    return path;
#else
    size_t size = 256;
    for (;;) {
        char *path = malloc(size + 1); if (!path) return NULL;
        ssize_t n = readlink("/proc/self/exe", path, size);
        if (n < 0) { free(path); return NULL; }
        if ((size_t)n < size) { path[n] = 0; return path; }
        free(path); size *= 2;
    }
#endif
}
#endif
int oceanRun(char *const argv[]) {
    pid_t child = oceanSpawn(argv, NULL, 0);
    int status = 0;
    if (child < 0) { perror(argv[0]); return 1; }
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
void oceanTemporary(char *buffer, size_t capacity, const char *name) {
    const char *base = getenv("TMPDIR");
#ifdef _WIN32
    if (!base || !*base) base = getenv("TEMP");
#endif
    if (!base || !*base) base = "/tmp";
    snprintf(buffer, capacity, "%s/%s", base, name);
#ifdef _WIN32
    for (char *p = buffer; *p; p++) if (*p == '\\') *p = '/';
#endif
}
