#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "package.h"
#include "array_type.h"
#include "diagnostic.h"
#include "scanner.h"
#include "source_path.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef struct Allocation { void *data; struct Allocation *next; } Allocation;
typedef struct Dependency {
    char *name, *version, *replacement, *replacementVersion;
    struct Dependency *next;
} Dependency;
typedef struct Module {
    char *root, *name;
    Dependency *dependencies;
    struct Module *next;
} Module;
typedef struct Package Package;
typedef struct Symbol Symbol;
typedef struct Import { char *alias; Package *package; struct Import *next; } Import;
typedef struct File {
    parseResult ast;
    char *path, *text, *displayPath;
    Import *imports;
    Symbol *externs;
    size_t id;
    struct File *next;
} File;
struct Symbol {
    char *name, *qualified;
    stmt *declaration;
    struct Symbol *next;
};
struct Package {
    char *directory, *name;
    size_t id, order;
    int state;
    Module *module;
    File *files;
    Symbol *symbols;
    struct Package *next;
};
struct packageBuild {
    const packageSource *editorSources;
    size_t nEditorSources;
    parseResult program;
    Package *packages, *root;
    Module *modules;
    Allocation *allocations;
    size_t nPackages, nFinished, nFiles;
    int errors;
};
static void *alloc(packageBuild *b, size_t size) {
    void *data = calloc(1, size ? size : 1);
    Allocation *allocation = malloc(sizeof(*allocation));
    if (!data || !allocation) { fprintf(stderr, "out of memory\n"); exit(1); }
    *allocation = (Allocation){data, b->allocations};
    b->allocations = allocation;
    return data;
}
static char *pkgString(packageBuild *b, const char *s) {
    char *copy = alloc(b, strlen(s) + 1);
    strcpy(copy, s);
    return copy;
}
static char *format(packageBuild *b, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt); va_copy(copy, args);
    int length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *result = alloc(b, (size_t)length + 1);
    vsnprintf(result, (size_t)length + 1, fmt, args);
    va_end(args);
    return result;
}
static void error(packageBuild *b, const char *file, const char *fmt, ...) {
    char *display = relativeSourcePath(file ? file : "ocean");
    token at = {.filename = display ? display : (char *)(file ? file : "ocean"), .line = 1, .column = 1};
    const char *source = NULL;
    for (Package *package = b->packages; package; package = package->next)
        for (File *entry = package->files; entry; entry = entry->next)
            if (file && !strcmp(entry->path, file)) source = entry->text;
    diagnosticStart(at, "package", "error");
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    diagnosticEnd(source, at);
    free(display); b->errors++;
}
static int directory(const char *path) {
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}
static char *readFile(packageBuild *b, const char *path) {
    for (size_t i = 0; i < b->nEditorSources; i++)
        if (!strcmp(path, b->editorSources[i].path)) return pkgString(b, b->editorSources[i].text);
    FILE *file = fopen(path, "rb");
    struct stat info;
    if (!file || fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size < 0) {
        if (file) fclose(file);
        error(b, path, "cannot read file: %s", strerror(errno)); return NULL;
    }
    char *text = alloc(b, (size_t)info.st_size + 1);
    if (fread(text, 1, (size_t)info.st_size, file) != (size_t)info.st_size || ferror(file)) {
        error(b, path, "cannot read file"); fclose(file); return NULL;
    }
    fclose(file); return text;
}
static char *canonical(packageBuild *b, const char *path) {
    char *result = realpath(path, NULL);
    if (!result) return NULL;
    char *copy = pkgString(b, result); free(result); return copy;
}
static char *parent(packageBuild *b, const char *path) {
    char *result = pkgString(b, path), *slash = strrchr(result, '/');
    if (slash == result) result[1] = 0;
    else if (slash) *slash = 0;
    return result;
}
static int prefix(const char *path, const char *base) {
    size_t n = strlen(base);
    return !strncmp(path, base, n) && (!path[n] || path[n] == '/');
}
int validModulePath(const char *path) {
    if (!*path || *path == '/' || strchr(path, '\\')) return 0;
    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && !strncmp(p, "..", 2))) return 0;
        for (size_t i = 0; i < n; i++)
            if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
                  (p[i] >= '0' && p[i] <= '9') || strchr("._-~", p[i]))) return 0;
        if (!end) break;
        p = end + 1;
        if (!*p) return 0;
    }
    return 1;
}
static Dependency *dependency(packageBuild *b, Module *module, const char *name) {
    for (Dependency *d = module->dependencies; d; d = d->next)
        if (!strcmp(d->name, name)) return d;
    Dependency *d = alloc(b, sizeof(*d));
    d->name = pkgString(b, name); d->next = module->dependencies; module->dependencies = d;
    return d;
}
/* go.mod-style directives, with quoted paths, comments, and require/replace blocks. */
static size_t words(char *line, char **out, size_t capacity) {
    size_t count = 0;
    char *p = line;
    while (*p && count < capacity) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == ';') p++;
        if (!*p || !strncmp(p, "//", 2) || *p == '#') break;
        if (*p == '"') {
            p++; out[count++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            out[count++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != ';') p++;
            if (*p) *p++ = 0;
        }
    }
    return count;
}
static Module *findModule(packageBuild *b, const char *location) {
    char *root = pkgString(b, location);
    while (1) {
        char *manifest = format(b, "%s/ocean.mod", root);
        if (!access(manifest, F_OK)) {
            for (Module *m = b->modules; m; m = m->next)
                if (!strcmp(m->root, root)) return m;
            Module *module = alloc(b, sizeof(*module));
            module->root = root; module->next = b->modules; b->modules = module;
            char *text = readFile(b, manifest), *save = NULL;
            int block = 0;
            for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
                char *part[10]; size_t n = words(line, part, 10);
                if (!n) continue;
                if (!strcmp(part[0], ")")) { block = 0; continue; }
                int directive = block;
                size_t start = 0;
                if (!strcmp(part[0], "module")) {
                    if (n != 2 || module->name || !validModulePath(part[1])) error(b, manifest, "invalid module declaration");
                    else module->name = pkgString(b, part[1]);
                    continue;
                }
                if (!strcmp(part[0], "go") || !strcmp(part[0], "ocean")) continue;
                if (!strcmp(part[0], "require") || !strcmp(part[0], "replace")) {
                    directive = !strcmp(part[0], "require") ? 1 : 2; start = 1;
                    if (n == 2 && !strcmp(part[1], "(")) { block = directive; continue; }
                }
                if (!directive || n <= start || !validModulePath(part[start])) {
                    error(b, manifest, "invalid module directive"); continue;
                }
                Dependency *d = dependency(b, module, part[start]);
                if (directive == 1) {
                    if (n != start + 2 || !validModulePath(part[start + 1])) error(b, manifest, "require expects a module path and version");
                    else d->version = pkgString(b, part[start + 1]);
                } else {
                    size_t arrow = start + 1;
                    if (arrow < n && strcmp(part[arrow], "=>")) arrow++;
                    if (arrow + 1 >= n || strcmp(part[arrow], "=>")) error(b, manifest, "replace expects module => directory or module version");
                    else {
                        d->replacement = pkgString(b, part[arrow + 1]);
                        if (arrow + 2 < n) d->replacementVersion = pkgString(b, part[arrow + 2]);
                    }
                }
            }
            if (!module->name) error(b, manifest, "missing module declaration");
            if (block) error(b, manifest, "unterminated directive block");
            return module;
        }
        if (!strcmp(root, "/")) return NULL;
        root = parent(b, root);
    }
}
static int mkdirs(const char *path) {
    char *copy = strdup(path);
    if (!copy) return 1;
    for (char *p = copy + 1; ; p++) {
        if (*p != '/' && *p) continue;
        char c = *p; *p = 0;
        if (mkdir(copy, 0700) && errno != EEXIST) { free(copy); return 1; }
        if (!c) break;
        *p = c;
    }
    free(copy); return 0;
}
static int process(char *const args[]) {
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) { execvp(args[0], args); perror(args[0]); _exit(127); }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return 1;
    return !WIFEXITED(status) || WEXITSTATUS(status);
}
static void removeTemporary(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            size_t n = strlen(path) + strlen(entry->d_name) + 2;
            char *child = malloc(n);
            if (!child) continue;
            snprintf(child, n, "%s/%s", path, entry->d_name);
            struct stat info;
            if (!lstat(child, &info) && S_ISDIR(info.st_mode)) removeTemporary(child);
            else unlink(child);
            free(child);
        }
        closedir(dir);
    }
    rmdir(path);
}
static char *download(packageBuild *b, const char *module, const char *version) {
    if (!validModulePath(module) || (version && (!validModulePath(version) || *version == '-'))) {
        error(b, module, "invalid module path or version"); return NULL;
    }
    const char *cache = getenv("OCEAN_MOD_CACHE"), *home = getenv("HOME");
    if (!cache) cache = format(b, "%s/.cache/ocean/modules", home ? home : "/tmp");
    char *destination = format(b, "%s/%s@%s", cache, module, version ? version : "latest");
    if (directory(destination)) return canonical(b, destination);
    char *base = parent(b, destination);
    if (mkdirs(base)) { error(b, base, "cannot create module cache"); return NULL; }
    char *temporary = format(b, "%s/.download-XXXXXX", base);
    if (!mkdtemp(temporary)) { error(b, base, "cannot create download directory"); return NULL; }
    char *url = format(b, "https://%s", module);
    fprintf(stderr, "ocean: downloading %s %s\n", module, version ? version : "latest");
    char *args[] = {"git", "clone", "--quiet", "--depth", "1", "--branch", (char *)version,
                    "--", url, temporary, NULL};
    char *latest[] = {"git", "clone", "--quiet", "--depth", "1", "--", url, temporary, NULL};
    if (process(version ? args : latest)) {
        error(b, module, "module download failed"); removeTemporary(temporary); return NULL;
    }
    if (access(format(b, "%s/ocean.mod", temporary), F_OK)) {
        error(b, module, "downloaded module has no ocean.mod"); removeTemporary(temporary); return NULL;
    }
    if (rename(temporary, destination)) {
        if (!directory(destination)) error(b, destination, "cannot install downloaded module");
        removeTemporary(temporary);
    }
    return canonical(b, destination);
}
static Package *loadDirectory(packageBuild *, const char *);
static char *standardSource(packageBuild *b) {
    const char *override = getenv("OCEAN_SRC");
    if (override) return pkgString(b, override);
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = alloc(b, size);
    if (_NSGetExecutablePath(path, &size)) return NULL;
    char *executable = canonical(b, path);
#else
    char *executable = NULL;
    for (size_t size = 256; ; size *= 2) {
        char *path = alloc(b, size + 1);
        ssize_t length = readlink("/proc/self/exe", path, size);
        if (length < 0) break;
        if ((size_t)length == size) continue;
        path[length] = 0;
        executable = path;
        break;
    }
#endif
    return executable ? format(b, "%s/../src", parent(b, executable)) : NULL;
}
static Package *resolveImport(packageBuild *b, Package *from, const char *path) {
    if (!validModulePath(path)) { error(b, from->directory, "invalid import path: %s", path); return NULL; }
    char *location = NULL;
    Module *module = from->module;
    if (module && module->name && prefix(path, module->name)) {
        const char *suffix = path + strlen(module->name);
        location = format(b, "%s%s", module->root, suffix);
    } else if (module && directory(format(b, "%s/%s", module->root, path))) {
        location = format(b, "%s/%s", module->root, path);
    } else {
        const char *standard = standardSource(b);
        if (!standard) {
            error(b, from->directory, "cannot locate compiler executable for standard library lookup");
            return NULL;
        }
        char *candidate = format(b, "%s/%s", standard, path);
        if (directory(candidate)) location = candidate;
    }
    if (!location) {
        Dependency *match = NULL;
        for (Dependency *d = module ? module->dependencies : NULL; d; d = d->next)
            if (prefix(path, d->name) && (!match || strlen(d->name) > strlen(match->name))) match = d;
        char *name = match ? match->name : NULL;
        if (!name) {
            const char *slash = strchr(path, '/');
            if (!slash || !memchr(path, '.', (size_t)(slash - path))) {
                error(b, from->directory, "package not found: %s", path); return NULL;
            }
            // Conventional forge paths identify a repository with host/owner/repo.
            const char *end = strchr(slash + 1, '/');
            if (end) end = strchr(end + 1, '/');
            name = pkgString(b, path);
            if (end) name[end - path] = 0;
        }
        char *root;
        if (match && match->replacement &&
            (match->replacement[0] == '.' || match->replacement[0] == '/')) {
            root = canonical(b, match->replacement[0] == '/' ? match->replacement :
                             format(b, "%s/%s", module->root, match->replacement));
        } else root = download(b, match && match->replacement ? match->replacement : name,
                               match && match->replacementVersion ? match->replacementVersion :
                               match ? match->version : NULL);
        if (!root) { error(b, from->directory, "cannot resolve module: %s", name); return NULL; }
        Module *downloaded = findModule(b, root);
        if (!downloaded || strcmp(downloaded->root, root)) {
            error(b, root, "module root must contain ocean.mod"); return NULL;
        }
        if (!(match && match->replacement) && downloaded->name && strcmp(downloaded->name, name)) {
            error(b, root, "module declares %s, expected %s", downloaded->name, name); return NULL;
        }
        location = format(b, "%s%s", root, path + strlen(name));
    }
    char *resolved = canonical(b, location);
    if (!resolved || !directory(resolved)) { error(b, location, "package directory does not exist"); return NULL; }
    return loadDirectory(b, resolved);
}
// Privilege follows the configured standard-library path, never a package name.
// Resolve every source file too: a symlink cannot import privileged source from
// an ordinary project or dependency into the standard reflect directory.
static int standardPackageSource(packageBuild *b, Package *p, File *file, const char *name) {
    const char *source = standardSource(b);
    char *root = source ? canonical(b, source) : NULL;
    if (!root) return 0;
    char *expected = format(b, "%s/%s", root, name);
    if (strcmp(p->directory, expected) || !p->name || strcmp(p->name, name)) return 0;
    char *resolved = canonical(b, file->path);
    return resolved && !strcmp(parent(b, resolved), expected);
}
static void markReflectionIntrinsic(packageBuild *b, Package *p, File *file, struct stmt_func *f) {
    if (!standardPackageSource(b, p, file, "reflect")) return;
    int fields = !strcmp(f->name.lexeme, "Fields"), predicate = !strcmp(f->name.lexeme, "IsStruct");
    if (!fields && !predicate) return;
    typeTokens element;
    int valid = !f->isExtern && !f->isVariadic && !f->receiverType.nTokens && !f->nTypeParams &&
                f->nParams == 1 && f->paramTypeNames[0].nTokens == 1 &&
                f->paramTypeNames[0].tokens[0].type == ANY && f->nResults == 1;
    if (valid && fields)
        valid = unsizedArrayTypeParts(f->resultTypes[0], &element) && element.nTokens == 1 &&
                !strcmp(element.tokens[0].lexeme, "Field");
    if (valid && predicate)
        valid = f->resultTypes[0].nTokens == 1 && !strcmp(f->resultTypes[0].tokens[0].lexeme, "bool");
    if (!valid) { error(b, file->path, "invalid standard reflect intrinsic signature: %s", f->name.lexeme); return; }
    f->compilerIntrinsic = fields ? INTRINSIC_REFLECT_FIELDS : INTRINSIC_REFLECT_IS_STRUCT;
}
static int compareNames(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static Symbol *symbol(Package *package, const char *name) {
    for (Symbol *s = package->symbols; s; s = s->next) if (!strcmp(s->name, name)) return s;
    return NULL;
}
static int sameSpan(typeTokens a, typeTokens b) {
    if (a.nTokens != b.nTokens) return 0;
    for (size_t i = 0; i < a.nTokens; i++)
        if (a.tokens[i].type != b.tokens[i].type || strcmp(a.tokens[i].lexeme, b.tokens[i].lexeme)) return 0;
    return 1;
}
static int sameExtern(struct stmt_func *a, struct stmt_func *b) {
    if (!a->isExtern || !b->isExtern || a->nParams != b->nParams || a->nResults != b->nResults ||
        a->isVariadic != b->isVariadic) return 0;
    for (size_t i = 0; i < a->nParams; i++) if (!sameSpan(a->paramTypeNames[i], b->paramTypeNames[i])) return 0;
    for (size_t i = 0; i < a->nResults; i++) if (!sameSpan(a->resultTypes[i], b->resultTypes[i])) return 0;
    return 1;
}
static void addSymbol(packageBuild *b, Package *package, stmt *declaration, token name) {
    if (name.type == UNDERSCORE) return;
    Symbol *existing = symbol(package, name.lexeme);
    if (existing) {
        error(b, name.filename, "duplicate package declaration: %s", name.lexeme); return;
    }
    Symbol *s = alloc(b, sizeof(*s));
    s->name = name.lexeme; s->declaration = declaration;
    s->qualified = format(b, "__pkg%zu_%s", package->id, name.lexeme);
    s->next = package->symbols; package->symbols = s;
}
static Package *loadDirectory(packageBuild *b, const char *location) {
    for (Package *p = b->packages; p; p = p->next)
        if (!strcmp(p->directory, location)) {
            if (p->state == 1) error(b, location, "import cycle");
            return p;
        }
    Package *p = alloc(b, sizeof(*p));
    p->id = b->nPackages++; p->directory = pkgString(b, location); p->state = 1;
    p->module = findModule(b, location);
    p->next = b->packages; b->packages = p;
    DIR *dir = opendir(location);
    if (!dir) { error(b, location, "cannot read package directory"); return p; }
    size_t count = 0, capacity = 16;
    char **names = malloc(capacity * sizeof(*names));
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 3, ".oc")) continue;
        char *path = format(b, "%s/%s", location, entry->d_name);
        struct stat info;
        if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
        if (count == capacity) { capacity *= 2; names = realloc(names, capacity * sizeof(*names)); }
        names[count++] = path;
    }
    closedir(dir);
    for (size_t i = 0; i < b->nEditorSources; i++) {
        const char *path = b->editorSources[i].path;
        const char *slash = strrchr(path, '/');
        size_t length = strlen(path);
        if (!slash || (size_t)(slash - path) != strlen(location) ||
            strncmp(path, location, strlen(location)) || length < 3 || strcmp(path + length - 3, ".oc")) continue;
        size_t j = 0;
        while (j < count && strcmp(names[j], path)) j++;
        if (j < count) continue;
        if (count == capacity) { capacity *= 2; names = realloc(names, capacity * sizeof(*names)); }
        names[count++] = pkgString(b, path);
    }
    qsort(names, count, sizeof(*names), compareNames);
    if (!count) error(b, location, "directory contains no Ocean source files");
    File **tail = &p->files;
    for (size_t i = 0; i < count; i++) {
        File *file = alloc(b, sizeof(*file));
        file->id = b->nFiles++;
        file->path = names[i]; file->text = readFile(b, file->path);
        *tail = file; tail = &file->next;
        if (!file->text) continue;
        char *display = relativeSourcePath(file->path);
        file->displayPath = pkgString(b, display ? display : file->path);
        free(display);
        tokenlist *tokens = scanTokens(file->text, file->displayPath);
        file->ast = parseTokensWithSource(tokens, file->text); free(tokens);
        file->ast.sourceFilename = file->displayPath; file->ast.sourceText = file->text;
        if (file->ast.hasError) { b->errors++; continue; }
        if (!file->ast.packageName) { error(b, file->path, "expected package declaration at the top of the file"); continue; }
        const char *packageName = file->ast.packageName;
        int validName = (*packageName >= 'a' && *packageName <= 'z') ||
                        (*packageName >= 'A' && *packageName <= 'Z') || *packageName == '_';
        for (const char *c = packageName; validName && *c; c++)
            if (!( (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                   (*c >= '0' && *c <= '9') || *c == '_')) validName = 0;
        if (!validName) error(b, file->path, "package name must be an identifier");
        if (!p->name) p->name = file->ast.packageName;
        else if (strcmp(p->name, file->ast.packageName)) error(b, file->path, "package %s conflicts with package %s", file->ast.packageName, p->name);
        for (size_t j = 0; j < file->ast.nStatements; j++) {
            stmt *s = &file->ast.statements[j];
            if (s->type == STMT_FUNC) {
                struct stmt_func *f = s->value;
                markReflectionIntrinsic(b, p, file, f);
                if (!strcmp(f->name.lexeme, "setArgs") && standardPackageSource(b, p, file, "os")) {
                    typeTokens stream = f->nParams ? f->paramTypeNames[0] : (typeTokens){0};
                    int valid = !f->isExtern && !f->isVariadic && !f->receiverType.nTokens &&
                        !f->nTypeParams && f->nParams == 2 && !f->nResults &&
                        stream.nTokens == 4 && !strcmp(stream.tokens[0].lexeme, "buffer") &&
                        stream.tokens[1].type == LESS_THAN && !strcmp(stream.tokens[2].lexeme, "i8") &&
                        stream.tokens[3].type == GREATER_THAN &&
                        f->paramTypeNames[1].nTokens == 1 && !strcmp(f->paramTypeNames[1].tokens[0].lexeme, "int");
                    if (!valid) error(b, file->path, "invalid standard os.setArgs signature");
                    else b->program.osSetArgs = f;
                }
                if (f->isExtern) {
                    Symbol *existing = file->externs;
                    while (existing && strcmp(existing->name, f->name.lexeme)) existing = existing->next;
                    if (existing) {
                        if (sameExtern(existing->declaration->value, f)) s->type = STMT_NULL;
                        else error(b, file->path, "conflicting extern declarations: %s", f->name.lexeme);
                    } else {
                        Symbol *symbol = alloc(b, sizeof(*symbol));
                        symbol->name = f->name.lexeme; symbol->declaration = s;
                        symbol->qualified = format(b, "__extern.%zu.%s", file->id, f->name.lexeme);
                        symbol->next = file->externs; file->externs = symbol;
                    }
                } else if (!f->receiverType.nTokens) addSymbol(b, p, s, f->name);
            } else if (s->type == STMT_STRUCT) addSymbol(b, p, s, ((struct stmt_struct *)s->value)->name);
            else if (s->type == STMT_INTERFACE) addSymbol(b, p, s, ((struct stmt_interface *)s->value)->name);
            else if (s->type == STMT_VAR) {
                struct stmt_var *v = s->value;
                for (size_t k = 0; k < v->nVars; k++) addSymbol(b, p, s, v->vars[k]);
            } else if (s->type == STMT_TYPE) addSymbol(b, p, s, ((struct stmt_type *)s->value)->name);
            else if (s->type == STMT_ENUM) {
                struct stmt_enum *e = s->value;
                addSymbol(b, p, s, e->name);

            }
        }
    }
    free(names);
    if (b->errors) { p->order = b->nFinished++; p->state = 2; return p; }
    for (File *file = p->files; file; file = file->next)
        for (size_t i = 0; i < file->ast.nStatements; i++) {
            stmt *s = &file->ast.statements[i];
            if (s->type != STMT_IMPORT) continue;
            struct stmt_import *node = s->value;
            Package *imported = resolveImport(b, p, node->path);
            if (!imported) continue;
            char *alias = node->importedName ? node->importedName : imported->name;
            if (!alias) continue;
            for (Import *old = file->imports; old; old = old->next)
                if (!strcmp(old->alias, alias) && strcmp(alias, "_") && strcmp(alias, "."))
                    error(b, file->path, "duplicate import name: %s", alias);
            if (symbol(p, alias)) error(b, file->path, "import name collides with package declaration: %s", alias);
            Import *import = alloc(b, sizeof(*import));
            *import = (Import){alias, imported, file->imports}; file->imports = import;
        }
    p->order = b->nFinished++;
    p->state = 2;
    return p;
}

typedef struct Local { const char *name; struct Local *next; } Local;
typedef struct Rewrite { packageBuild *build; Package *package; File *file; } Rewrite;
static int local(Local *scope, const char *name) {
    for (; scope; scope = scope->next) if (!strcmp(scope->name, name)) return 1;
    return 0;
}
static Local *bindLocal(Rewrite *r, Local *scope, token name) {
    if (!name.lexeme || name.type == EOF_TOKEN) return scope;
    Local *entry = alloc(r->build, sizeof(*entry));
    *entry = (Local){name.lexeme, scope}; return entry;
}
static Symbol *resolveName(Rewrite *r, Local *scope, const char *name) {
    if (local(scope, name)) return NULL;
    Symbol *found = symbol(r->package, name);
    if (found) return found;
    for (Import *i = r->file->imports; i; i = i->next) {
        if (strcmp(i->alias, ".")) continue;
        Symbol *candidate = symbol(i->package, name);
        if (candidate && !exportedName(candidate->name)) continue;
        if (candidate && found && candidate != found) error(r->build, r->file->path, "ambiguous dot import name: %s", name);
        if (candidate) found = candidate;
    }
    return found;
}
static Package *importName(Rewrite *r, Local *scope, const char *name) {
    if (local(scope, name)) return NULL;
    for (Import *i = r->file->imports; i; i = i->next)
        if (!strcmp(i->alias, name)) return i->package;
    return NULL;
}
static Symbol *importedSymbol(Rewrite *r, Package *package, token name, const char *qualifier) {
    Symbol *found = symbol(package, name.lexeme);
    if (!found || !exportedName(found->name)) {
        diagnostic(r->file->text, name, "package", "error",
                   found ? "declaration %s is not exported by package %s" : "package %s has no declaration %s",
                   found ? name.lexeme : qualifier, found ? qualifier : name.lexeme);
        r->build->errors++;
        return NULL;
    }
    return found;
}
static void rewriteToken(Rewrite *r, Local *scope, token *name) {
    if (name->type != IDENTIFIER || !name->lexeme) return;
    Symbol *s = resolveName(r, scope, name->lexeme);
    if (s) name->lexeme = s->qualified;
}
static void rewriteType(Rewrite *r, Local *scope, typeTokens *type) {
    if (!type->nTokens) return;
    // Spans may share token storage; copy before rewriting and collapsing pkg.Type.
    // Like parser-created type tokens, this storage belongs to the AST.
    // Semantic analysis may free it when resolving an ambiguous index expression.
    token *tokens = malloc(sizeof(token) * type->nTokens);
    if (!tokens) { fprintf(stderr, "out of memory\n"); exit(1); }
    size_t count = 0;
    for (size_t i = 0; i < type->nTokens; i++) {
        token name = type->tokens[i];
        if (name.type == EXTERN && i + 2 < type->nTokens && type->tokens[i + 1].type == DOT) {
            token member = type->tokens[i + 2];
            Symbol *symbol = r->file->externs;
            while (symbol && strcmp(symbol->name, member.lexeme)) symbol = symbol->next;
            if (!symbol) error(r->build, r->file->path, "extern function is not declared in this file: %s", member.lexeme);
            else member.lexeme = symbol->qualified;
            tokens[count++] = name;
            tokens[count++] = type->tokens[i + 1];
            tokens[count++] = member;
            i += 2;
            continue;
        }
        Package *imported = name.type == IDENTIFIER ? importName(r, scope, name.lexeme) : NULL;
        if (imported && i + 2 < type->nTokens && type->tokens[i + 1].type == DOT) {
            token member = type->tokens[i + 2];
            Symbol *s = importedSymbol(r, imported, member, name.lexeme);
            if (s) { member.lexeme = s->qualified; tokens[count++] = member; i += 2; continue; }
        }
        if (!(name.type == IDENTIFIER && !strcmp(name.lexeme, "array") &&
              (i == 0 || type->tokens[i - 1].type == LESS_THAN ||
               type->tokens[i - 1].type == STAR || type->tokens[i - 1].type == BITWISE_OR)))
            if (!i || type->tokens[i - 1].type != DOT) rewriteToken(r, scope, &name);
        tokens[count++] = name;
    }
    *type = (typeTokens){count, tokens};
}
static void rewriteExpr(Rewrite *, Local *, expr *);
static Local *rewriteStmt(Rewrite *, Local *, stmt *, int);
static void rewriteBlock(Rewrite *r, Local *scope, struct stmt_block *block) {
    for (size_t i = 0; i < block->nStmts; i++) scope = rewriteStmt(r, scope, &block->stmts[i], 0);
}
static void rewriteFunction(Rewrite *r, Local *outer, struct stmt_func *f, int top) {
    if (top && !f->receiverType.nTokens) {
        f->displayName = f->name.lexeme;
        if (f->isExtern) {
            f->externalName = f->name.lexeme;
            for (Symbol *symbol = r->file->externs; symbol; symbol = symbol->next)
                if (!strcmp(symbol->name, f->name.lexeme)) { f->name.lexeme = symbol->qualified; break; }
        } else rewriteToken(r, NULL, &f->name);
    }
    Local *scope = outer;
    for (size_t i = 0; i < f->nTypeParams; i++) scope = bindLocal(r, scope, f->typeParamNames[i]);
    for (size_t i = 0; i < f->nReceiverTypeParams; i++) scope = bindLocal(r, scope, f->receiverTypeParamNames[i]);
    scope = bindLocal(r, scope, f->receiverName);
    for (size_t i = 0; i < f->nParams; i++) scope = bindLocal(r, scope, f->paramNames[i]);
    for (size_t i = 0; i < f->nResults; i++) scope = bindLocal(r, scope, f->resultNames[i]);
    rewriteType(r, scope, &f->receiverType);
    rewriteType(r, scope, &f->type);
    for (size_t i = 0; i < f->nParams; i++) rewriteType(r, scope, &f->paramTypeNames[i]);
    for (size_t i = 0; i < f->nResults; i++) rewriteType(r, scope, &f->resultTypes[i]);
    for (size_t i = 0; i < f->nTypeParams; i++) rewriteType(r, scope, &f->typeParamConstraints[i]);
    rewriteBlock(r, scope, &f->body);
}
static void rewriteExpr(Rewrite *r, Local *scope, expr *e) {
    if (!e || !e->underlying) return;
    switch (e->type) {
    case EXPR_TYPE_IDENTIFIER: {
        struct expr_identifier *identifier = e->underlying;
        if (identifier->externReference) {
            Symbol *symbol = r->file->externs;
            while (symbol && strcmp(symbol->name, identifier->lexeme.lexeme)) symbol = symbol->next;
            if (!symbol) error(r->build, r->file->path, "extern function is not declared in this file: %s", identifier->lexeme.lexeme);
            else identifier->lexeme.lexeme = symbol->qualified;
        } else rewriteToken(r, scope, &identifier->lexeme);
        break;
    }
    case EXPR_TYPE_DOT: {
        struct expr_dot *v = e->underlying;
        if (v->parent.type == EXPR_TYPE_IDENTIFIER && v->field.type == EXPR_TYPE_IDENTIFIER) {
            token name = ((struct expr_identifier *)v->parent.underlying)->lexeme;
            Package *imported = importName(r, scope, name.lexeme);
            if (imported) {
                token member = ((struct expr_identifier *)v->field.underlying)->lexeme;
                Symbol *s = importedSymbol(r, imported, member, name.lexeme);
                if (s) {
                    struct expr_identifier *identifier = alloc(r->build, sizeof(*identifier));
                    identifier->lexeme = member; identifier->lexeme.lexeme = s->qualified;
                    e->type = EXPR_TYPE_IDENTIFIER; e->underlying = identifier;
                }
                break;
            }
        }
        rewriteExpr(r, scope, &v->parent); break;
    }
    case EXPR_TYPE_BINARY: {
        struct expr_binary *v = e->underlying;
        rewriteExpr(r, scope, &v->left); rewriteExpr(r, scope, &v->right); break;
    }
    case EXPR_TYPE_UNARY: {
        struct expr_unary *v = e->underlying;
        rewriteExpr(r, scope, &v->child); rewriteType(r, scope, &v->sizeofType); break;
    }
    case EXPR_TYPE_TERNARY: {
        struct expr_ternary *v = e->underlying;
        rewriteExpr(r, scope, &v->condition); rewriteExpr(r, scope, &v->ifTrue); rewriteExpr(r, scope, &v->ifFalse); break;
    }
    case EXPR_TYPE_CALL: {
        struct expr_call *v = e->underlying;
        rewriteExpr(r, scope, &v->func);
        for (size_t i = 0; i < v->nParams; i++) rewriteExpr(r, scope, &v->params[i]);
        break;
    }
    case EXPR_TYPE_INDEX: {
        struct expr_index *v = e->underlying;
        rewriteExpr(r, scope, &v->parent); rewriteExpr(r, scope, &v->index); break;
    }
    case EXPR_TYPE_INSTANTIATION: {
        struct expr_instantiation *v = e->underlying;
        rewriteExpr(r, scope, &v->parent);
        for (size_t i = 0; i < v->nTypeArguments; i++) rewriteType(r, scope, &v->typeArguments[i]);
        break;
    }
    case EXPR_TYPE_COMPOSITE: {
        struct expr_composite *v = e->underlying;
        rewriteExpr(r, scope, &v->type); rewriteType(r, scope, &v->literalType);
        for (size_t i = 0; i < v->nEntries; i++) {
            if (v->keys[i].type != EXPR_TYPE_IDENTIFIER) rewriteExpr(r, scope, &v->keys[i]);
            rewriteExpr(r, scope, &v->values[i]);
        }
        break;
    }
    case EXPR_TYPE_TYPECAST: {
        struct expr_typecast *v = e->underlying;
        rewriteExpr(r, scope, &v->child); rewriteType(r, scope, &v->type); break;
    }
    case EXPR_TYPE_ASSERTION: {
        struct expr_assertion *v = e->underlying;
        rewriteExpr(r, scope, &v->child); rewriteType(r, scope, &v->type); break;
    }
    case EXPR_TYPE_ARRAY_NEW: {
        struct expr_array_new *v = e->underlying;
        rewriteType(r, scope, &v->elementType); rewriteExpr(r, scope, &v->count); break;
    }
    case EXPR_TYPE_TYPE_NAME:
    case EXPR_TYPE_MAKE: case EXPR_TYPE_NEW:
    case EXPR_TYPE_ZERO: rewriteType(r, scope, &((struct expr_zero *)e->underlying)->type); break;
    case EXPR_TYPE_FUNCTION: rewriteFunction(r, scope, e->underlying, 0); break;
    default: break;
    }
}
static Local *rewriteStmt(Rewrite *r, Local *scope, stmt *s, int top) {
    if (!s) return scope;
    switch (s->type) {
    case STMT_VAR: {
        struct stmt_var *v = s->value;
        rewriteType(r, scope, &v->type);
        for (size_t i = 0; i < v->nInitValues; i++) rewriteExpr(r, scope, &v->initValues[i]);
        for (size_t i = 0; i < v->nVars; i++) {
            if (top) rewriteToken(r, NULL, &v->vars[i]);
            else scope = bindLocal(r, scope, v->vars[i]);
        }
        break;
    }
    case STMT_STRUCT: {
        struct stmt_struct *v = s->value;
        if (top) rewriteToken(r, NULL, &v->name);
        else scope = bindLocal(r, scope, v->name);
        Local *inner = scope;
        for (size_t i = 0; i < v->nTypeParams; i++) inner = bindLocal(r, inner, v->typeParamNames[i]);
        for (size_t i = 0; i < v->nFields; i++) inner = bindLocal(r, inner, v->fieldNames[i]);
        for (size_t i = 0; i < v->nTypeParams; i++) rewriteType(r, inner, &v->typeParamConstraints[i]);
        for (size_t i = 0; i < v->nFields; i++) rewriteType(r, inner, &v->fieldTypes[i]);
        for (size_t i = 0; i < v->nOperatorMethods; i++) rewriteFunction(r, inner, &v->operatorMethods[i], 0);
        break;
    }
    case STMT_FUNC:
        if (!top) scope = bindLocal(r, scope, ((struct stmt_func *)s->value)->name);
        rewriteFunction(r, scope, s->value, top); break;
    case STMT_BLOCK: rewriteBlock(r, scope, s->value); break;
    case STMT_EXPR: case STMT_PANIC: case STMT_RETURN_PANIC: rewriteExpr(r, scope, s->value); break;
    case STMT_RETURN: {
        struct stmt_return *v = s->value;
        for (size_t i = 0; i < v->nValues; i++) rewriteExpr(r, scope, &v->values[i]);
        break;
    }
    case STMT_IF: {
        struct stmt_if *v = s->value;
        Local *inner = rewriteStmt(r, scope, v->preStmt, 0);
        rewriteExpr(r, inner, v->condition);
        rewriteStmt(r, inner, v->ifTrue, 0); rewriteStmt(r, inner, v->ifFalse, 0); break;
    }
    case STMT_FOR: {
        struct stmt_for *v = s->value;
        Local *inner = rewriteStmt(r, scope, v->initializer, 0);
        rewriteExpr(r, inner, v->range);
        for (size_t i = 0; i < v->nBindings; i++) inner = bindLocal(r, inner, v->bindings[i]);
        rewriteExpr(r, inner, v->condition); rewriteExpr(r, inner, v->update);
        rewriteStmt(r, inner, v->body, 0); break;
    }
    case STMT_LABEL: scope = rewriteStmt(r, scope, ((struct stmt_label *)s->value)->body, 0); break;
    case STMT_ASM: {
        struct stmt_asm *v = s->value;
        for (size_t i=0; i<v->nOperands; i++) rewriteToken(r, scope, &v->operands[i]);
        break;
    }
    case STMT_DEFER: rewriteExpr(r, scope, ((struct stmt_defer *)s->value)->call); break;
    case STMT_ENUM: {
        struct stmt_enum *v = s->value;
        if (top) rewriteToken(r, NULL, &v->name); else scope = bindLocal(r, scope, v->name);
        Local *inner = scope;
        for (size_t i = 0; i < v->nTypeParams; i++) inner = bindLocal(r, inner, v->typeParamNames[i]);
        for (size_t i = 0; i < v->nTypeParams; i++) rewriteType(r, inner, &v->typeParamConstraints[i]);
        for (size_t i = 0; i < v->nMembers; i++) {
            rewriteExpr(r, inner, v->values[i]);
            rewriteType(r, inner, &v->valueTypes[i]);
            for (size_t j = 0; j < v->payloads[i].nFields; j++) rewriteType(r, inner, &v->payloads[i].fieldTypes[j]);
        }
        break;
    }
    case STMT_INTERFACE: {
        struct stmt_interface *v = s->value;
        if (top) rewriteToken(r, NULL, &v->name); else scope = bindLocal(r, scope, v->name);
        Local *inner = scope;
        for (size_t i = 0; i < v->nTypeParams; i++) inner = bindLocal(r, inner, v->typeParamNames[i]);
        for (size_t i = 0; i < v->nTypeParams; i++) rewriteType(r, inner, &v->typeParamConstraints[i]);
        for (size_t i = 0; i < v->nConstraints; i++) rewriteType(r, inner, &v->constraints[i]);
        for (size_t i = 0; i < v->nMethods; i++) rewriteFunction(r, inner, &v->methods[i], 0);
        for (size_t i = 0; i < v->nFields; i++) rewriteType(r, inner, &v->fieldTypes[i]);
        break;
    }
    case STMT_SWITCH: {
        struct stmt_switch *v = s->value;
        Local *inner = rewriteStmt(r, scope, &v->initializer, 0);
        rewriteExpr(r, inner, v->value);
        if (v->isTypeSwitch) inner = bindLocal(r, inner, v->binding);
        for (size_t i = 0; i < v->nCases; i++) {
            Local *branch = inner;
            for (size_t j = 0; j < v->cases[i].nValues; j++) {
                expr *pattern = &v->cases[i].values[j];
                if (!v->isMatch) { rewriteExpr(r, inner, pattern); continue; }
                expr *head = pattern, *items = NULL; size_t count = 0;
                if (pattern->type == EXPR_TYPE_CALL) {
                    struct expr_call *p = pattern->underlying; head = &p->func; items = p->params; count = p->nParams;
                } else if (pattern->type == EXPR_TYPE_COMPOSITE) {
                    struct expr_composite *p = pattern->underlying; head = &p->type; items = p->values; count = p->nEntries;
                }
                rewriteExpr(r, inner, head);
                for (size_t k = 0; k < count; k++) {
                    expr *binding = &items[k];
                    if (binding->type == EXPR_TYPE_BINARY) binding = &((struct expr_binary *)binding->underlying)->left;
                    if (binding->type == EXPR_TYPE_IDENTIFIER) branch = bindLocal(r, branch, ((struct expr_identifier *)binding->underlying)->lexeme);
                }
            }
            for (size_t j = 0; j < v->cases[i].nTypes; j++) rewriteType(r, inner, &v->cases[i].types[j]);
            rewriteBlock(r, branch, &v->cases[i].body);
        }
        break;
    }
    case STMT_TYPE: {
        struct stmt_type *v = s->value;
        rewriteType(r, scope, &v->underlying);
        if (top) rewriteToken(r, NULL, &v->name); else scope = bindLocal(r, scope, v->name);
        break;
    }
    default: break;
    }
    return scope;
}
packageBuild *loadPackage(const char *location) {
    return loadPackageWithSources(location, NULL, 0);
}
packageBuild *loadPackageWithSources(const char *location, const packageSource *sources, size_t count) {
    packageBuild *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->editorSources = sources; b->nEditorSources = count;
    char *path = canonical(b, location);
    if (!path) { error(b, location, "location does not exist"); return b; }
    if (!directory(path)) {
        size_t n = strlen(path);
        if (n < 3 || strcmp(path + n - 3, ".oc")) { error(b, location, "expected an Ocean file or package directory"); return b; }
        path = parent(b, path);
    }
    b->root = loadDirectory(b, path);
    if (b->errors || !b->root->name) return b;
    b->program.packageName = b->root->name;
    b->program.isLibrary = strcmp(b->root->name, "main") != 0;
    Symbol *entry = symbol(b->root, "main");
    if (!b->program.isLibrary && entry) entry->qualified = pkgString(b, "main");
    size_t statements = 0, sourceCount = 0;
    for (Package *p = b->packages; p; p = p->next)
        for (File *file = p->files; file; file = file->next) { statements += file->ast.nStatements; sourceCount++; }
    b->program.statements = alloc(b, sizeof(stmt) * statements);
    b->program.sources = alloc(b, sizeof(sourceFile) * sourceCount);
    for (size_t order = 0; order < b->nPackages; order++)
      for (Package *p = b->packages; p; p = p->next) {
        if (p->order != order) continue;
        for (File *file = p->files; file; file = file->next) {
            b->program.sources[b->program.nSources++] = (sourceFile){file->displayPath, file->text, p->id + 1};
            Rewrite rewrite = {b, p, file};
            for (size_t i = 0; i < file->ast.nStatements; i++) {
                stmt *s = &file->ast.statements[i];
                if (s->type == STMT_IMPORT || s->type == STMT_PACKAGE) continue;
                rewriteStmt(&rewrite, NULL, s, 1);
                b->program.statements[b->program.nStatements++] = *s;
            }
        }
      }
    if (b->program.nSources) {
        b->program.sourceFilename = b->root->files->displayPath;
        b->program.sourceText = b->root->files->text;
    }
    return b;
}
parseResult *packagePartialProgram(packageBuild *b) {
    return b && b->program.nSources ? &b->program : NULL;
}
parseResult *packageProgram(packageBuild *b) {
    return b && !b->errors ? &b->program : NULL;
}
void freePackage(packageBuild *b) {
    if (!b) return;
    for (Package *p = b->packages; p; p = p->next)
        for (File *file = p->files; file; file = file->next) {
            free(file->ast.statements); free(file->ast.packageName);
        }
    while (b->allocations) {
        Allocation *a = b->allocations; b->allocations = a->next;
        free(a->data); free(a);
    }
    free(b);
}

const char *packageDirectory(packageBuild *build) {
    return build && build->root ? build->root->directory : NULL;
}

void visitPackageImports(packageBuild *build, packageImportVisitor visitor, void *context) {
    for(Package *p=build->packages;p;p=p->next)for(File *file=p->files;file;file=file->next)
        for(Import *i=file->imports;i;i=i->next)visitor(file->displayPath,i->alias,i->package->id+1,context);
}
