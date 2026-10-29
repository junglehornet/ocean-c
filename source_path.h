#ifndef OCEAN_SOURCE_PATH_H
#define OCEAN_SOURCE_PATH_H
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Presentation only: callers retain their original paths for file operations.
 * Runtime diagnostics embed this path relative to the compiler's cwd. */
static inline char *relativeSourcePath(const char *path) {
    if (!path) path = "<source>";
    if (*path != '/') {
        while (path[0] == '.' && path[1] == '/') path += 2;
        size_t size = strlen(path) + 1;
        char *result = malloc(size);
        if (result) memcpy(result, path, size);
        return result;
    }
    char *cwd = getcwd(NULL, 0);
    size_t common = 0, i = 0, parents = 0;
    if (cwd) {
        while (cwd[i] && path[i] && cwd[i] == path[i]) {
            if (path[i] == '/') common = i + 1;
            i++;
        }
        if (!cwd[i] && (!path[i] || path[i] == '/')) common = i + (path[i] == '/');
        size_t length = strlen(cwd);
        if (common < length)
            for (size_t j = common; j < length; j++)
                if (j == common || cwd[j - 1] == '/') parents++;
    }
    const char *suffix = cwd ? path + common : path;
    char *result = malloc(parents * 3 + strlen(suffix) + 2);
    if (result) {
        char *out = result;
        for (size_t j = 0; j < parents; j++) { memcpy(out, "../", 3); out += 3; }
        strcpy(out, suffix);
        if (!*suffix) {
            if (out != result) out[-1] = 0;
            else strcpy(result, ".");
        }
    }
    free(cwd);
    return result;
}
#endif
