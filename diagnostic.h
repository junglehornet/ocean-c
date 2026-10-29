#ifndef OCEAN_DIAGNOSTIC_H
#define OCEAN_DIAGNOSTIC_H
#include "token.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline int diagnosticColor(void) {
    if (getenv("NO_COLOR")) return 0;
    const char *force = getenv("CLICOLOR_FORCE");
    return (force && *force && strcmp(force, "0")) || isatty(STDERR_FILENO);
}
static inline void diagnosticStart(token at, const char *phase, const char *severity) {
    if (diagnosticColor()) fputs(!strcmp(severity, "warning") ? "\033[33m" : "\033[31m", stderr);
    fprintf(stderr, "[%s:%ld:%ld]: %s %s: ", at.filename ? at.filename : "<source>",
            at.line > 0 ? at.line : 1, at.column > 0 ? at.column : 1, phase, severity);
}
static inline void diagnosticSnippet(const char *source, token at) {
    if (!source) return;
    const char *line = source;
    for (long n = 1; n < at.line; n++) {
        const char *next = strchr(line, '\n');
        if (!next) return;
        line = next + 1;
    }
    size_t length = strcspn(line, "\r\n");
    char prefix[64];
    size_t width = (size_t)snprintf(prefix, sizeof(prefix), " %ld | ", at.line > 0 ? at.line : 1);
    fputs(prefix, stderr);
    fwrite(line, 1, length, stderr);
    fputc('\n', stderr);
    for (size_t i = 0; i < width; i++) fputc(prefix[i] == '|' ? '|' : ' ', stderr);
    size_t caret = at.column > 0 ? (size_t)(at.column - 1) : 0;
    if (caret > length) caret = length;
    for (size_t i = 0; i < caret; i++) fputc(line[i] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}
static inline void diagnosticEnd(const char *source, token at) {
    fputc('\n', stderr);
    diagnosticSnippet(source, at);
    if (diagnosticColor()) fputs("\033[0m", stderr);
}
static inline void diagnostic(const char *source, token at, const char *phase,
                              const char *severity, const char *format, ...) {
    diagnosticStart(at, phase, severity);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    diagnosticEnd(source, at);
}
#endif
