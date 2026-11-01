#define _POSIX_C_SOURCE 200809L
#include "llvm_packages.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *text, *name;
    size_t package;
} Part;
static char *append(char *s, const char *line) {
    size_t n = s ? strlen(s) : 0;
    char *p = realloc(s, n + strlen(line) + 1);
    if (!p) {
        free(s);
        return NULL;
    }
    strcpy(p + n, line);
    return p;
}
static char *symbol(const char *s) {
    const char *p = strchr(s, '@');
    if (!p)
        return NULL;
    const char *e = p + 1;
    if (*e == '"') {
        e++;
        while (*e && *e != '"') {
            if (*e == '\\' && e[1])
                e++;
            e++;
        }
        if (*e)
            e++;
    } else
        while (*e && (isalnum((unsigned char)*e) || strchr("._$-", *e)))
            e++;
    return strndup(p, e - p);
}
static int sameSymbol(const char *a, const char *b) {
    if (*a == '@')
        a++;
    if (*b == '@')
        b++;
    size_t na = strlen(a), nb = strlen(b);
    if (*a == '"' && na >= 2) {
        a++;
        na -= 2;
    }
    if (*b == '"' && nb >= 2) {
        b++;
        nb -= 2;
    }
    return na == nb && !memcmp(a, b, na);
}
static int references(const char *text, const char *name) {
    if (!text || !name)
        return 0;
    const char *p = text;
    while ((p = strchr(p, '@'))) {
        char *n = symbol(p);
        if (!n)
            return 0;
        int match = sameSymbol(n, name);
        size_t length = strlen(n);
        free(n);
        if (match)
            return 1;
        p += length ? length : 1;
    }
    return 0;
}
static char *publicText(const char *text) {
    char *p = strdup(text);
    if (!p)
        return NULL;
    char *where = strstr(p, "private ");
    size_t n = 8;
    if (!where) {
        where = strstr(p, "internal ");
        n = 9;
    }
    if (where) {
        size_t offset = where - p;
        char *r = malloc(strlen(p) + 1);
        if (!r) {
            free(p);
            return NULL;
        }
        memcpy(r, p, offset);
        strcpy(r + offset, "hidden ");
        strcpy(r + offset + 7, where + n);
        free(p);
        p = r;
    }
    return p;
}
static char *prototype(const char *text) {
    const char *end = strchr(text, '\n');
    if (!end)
        return NULL;
    char *s = strndup(text, end - text);
    if (!s)
        return NULL;
    char *brace = strrchr(s, '{');
    if (brace)
        *brace = 0;
    char *public = publicText(s);
    free(s);
    if (!public)
        return NULL;
    size_t n = strlen(public) + 3;
    char *r = malloc(n);
    if (r)
        snprintf(r, n, "declare %s\n", public + 7);
    free(public);
    return r;
}
static int add(Part **parts, size_t *count, char *text, size_t package) {
    if (!text)
        return 0;
    Part *p = realloc(*parts, (*count + 1) * sizeof(*p));
    if (!p) {
        free(text);
        return 0;
    }
    *parts = p;
    char *name = symbol(text);
    if (!name) {
        free(text);
        return 0;
    }
    p[*count] = (Part){text, name, package};
    (*count)++;
    return 1;
}
void freeLLVMPackages(llvmPackages *u) {
    for (size_t i = 0; i < u->count; i++) {
        unlink(u->paths[i]);
        free(u->paths[i]);
    }
    free(u->paths);
    if (u->directory) {
        rmdir(u->directory);
        free(u->directory);
    }
    memset(u, 0, sizeof(*u));
}
int splitLLVMPackages(parseResult *program, const char *path, llvmPackages *units) {
    FILE *in = fopen(path, "r");
    if (!in)
        return 1;
    Part *functions = NULL, *globals = NULL, *externs = NULL;
    size_t nf = 0, ng = 0, ne = 0, package = 0, root = 1;
    int failed = 0;
    char *types = strdup("; Ocean package object\n"), *line = NULL, *body = NULL;
    size_t capacity = 0;
    if (!types)
        failed = 1;
    for (size_t i = 0; i < program->nSources; i++)
        if (!strcmp(program->sources[i].filename, program->sourceFilename))
            root = program->sources[i].packageId;
    while (!failed && getline(&line, &capacity, in) >= 0) {
        if (body) {
            body = append(body, line);
            if (!body) {
                failed = 1;
                break;
            }
            if (!strcmp(line, "}\n")) {
                failed = !add(&functions, &nf, body, package);
                body = NULL;
                package = 0;
            }
            continue;
        }
        if (sscanf(line, "; ocean-package: %zu", &package) == 1)
            continue;
        if (!strncmp(line, "define ", 7)) {
            if (strstr(line, "@main("))
                package = root;
            body = strdup(line);
            if (!body)
                failed = 1;
        } else if (line[0] == '@')
            failed = !add(&globals, &ng, strdup(line), 0);
        else if (!strncmp(line, "declare ", 8))
            failed = !add(&externs, &ne, strdup(line), 0);
        else if (line[0] == '%' || !strncmp(line, "target ", 7) ||
                 !strncmp(line, "source_filename ", 16)) {
            types = append(types, line);
            if (!types)
                failed = 1;
        }
    }
    if (ferror(in) || body)
        failed = 1;
    free(body);
    free(line);
    fclose(in);
    size_t *ids = calloc(nf + 1, sizeof(*ids)), nids = 1;
    if (!ids)
        failed = 1;
    for (size_t i = 0; !failed && i < nf; i++) {
        int found = 0;
        for (size_t j = 0; j < nids; j++)
            if (ids[j] == functions[i].package)
                found = 1;
        if (!found)
            ids[nids++] = functions[i].package;
    }
    char directory[] = "/tmp/ocean-packages-XXXXXX";
    if (!failed && !mkdtemp(directory))
        failed = 1;
    if (!failed) {
        units->directory = strdup(directory);
        units->paths = calloc(nids, sizeof(char *));
        if (!units->directory || !units->paths) {
            if (!units->directory)
                rmdir(directory);
            failed = 1;
        }
    }
    for (size_t unit = 0; !failed && unit < nids; unit++) {
        char *owned = strdup("");
        if (!ids[unit])
            for (size_t i = 0; owned && i < ng; i++)
                owned = append(owned, globals[i].text);
        for (size_t i = 0; owned && i < nf; i++)
            if (functions[i].package == ids[unit])
                owned = append(owned, functions[i].text);
        char output[256];
        snprintf(output, sizeof(output), "%s/package-%zu.ll", directory, ids[unit]);
        FILE *out = owned ? fopen(output, "w") : NULL;
        if (!out) {
            free(owned);
            failed = 1;
            break;
        }
        char *savedPath = strdup(output);
        if (!savedPath) {
            fclose(out);
            unlink(output);
            free(owned);
            failed = 1;
            break;
        }
        units->paths[units->count++] = savedPath;
        fputs(types, out);
        for (size_t i = 0; i < ng; i++) {
            Part *g = &globals[i];
            if (!ids[unit]) {
                char *s = publicText(g->text);
                if (s) {
                    fputs(s, out);
                    free(s);
                } else
                    failed = 1;
            } else if (references(owned, g->name) && strncmp(g->name, "@llvm.", 6)) {
                if (strstr(g->text, "= external ")) {
                    fputs(g->text, out);
                    continue;
                }
                const char *kind = strstr(g->text, " constant ") ? "constant" : "global";
                fprintf(out, "%s = external hidden %s%s i8\n", g->name,
                        strstr(g->text, "thread_local ") ? "thread_local " : "", kind);
            }
        }
        for (size_t i = 0; i < ne; i++)
            if (references(owned, externs[i].name))
                fputs(externs[i].text, out);
        for (size_t i = 0; i < nf; i++)
            if (functions[i].package != ids[unit] && references(owned, functions[i].name)) {
                char *s = prototype(functions[i].text);
                if (s) {
                    fputs(s, out);
                    free(s);
                } else
                    failed = 1;
            }
        for (size_t i = 0; i < nf; i++)
            if (functions[i].package == ids[unit]) {
                char *s = publicText(functions[i].text);
                if (s) {
                    fputs(s, out);
                    free(s);
                } else
                    failed = 1;
            }
        if (ferror(out))
            failed = 1;
        if (fclose(out))
            failed = 1;
        free(owned);
    }
    for (size_t i = 0; i < nf; i++) {
        free(functions[i].text);
        free(functions[i].name);
    }
    free(functions);
    for (size_t i = 0; i < ng; i++) {
        free(globals[i].text);
        free(globals[i].name);
    }
    free(globals);
    for (size_t i = 0; i < ne; i++) {
        free(externs[i].text);
        free(externs[i].name);
    }
    free(externs);
    free(types);
    free(ids);
    if (failed)
        freeLLVMPackages(units);
    return failed;
}
