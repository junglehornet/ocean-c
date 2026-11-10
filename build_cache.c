#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "build_cache.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* SHA-256 keeps keys and data integrity independent of external utilities. */
typedef struct { uint32_t h[8]; uint64_t bytes; unsigned used; unsigned char block[64]; } Digest;
static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void compress(Digest *d) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)d->block[4*i] << 24 | (uint32_t)d->block[4*i+1] << 16 |
               (uint32_t)d->block[4*i+2] << 8 | d->block[4*i+3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (rotate(x,7) ^ rotate(x,18) ^ (x >> 3)) + w[i-7] +
               (rotate(y,17) ^ rotate(y,19) ^ (y >> 10));
    }
    uint32_t a=d->h[0], b=d->h[1], c=d->h[2], e=d->h[4], f=d->h[5], g=d->h[6], h=d->h[7], v=d->h[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t = h + (rotate(e,6)^rotate(e,11)^rotate(e,25)) + ((e&f)^(~e&g)) + k[i] + w[i];
        uint32_t u = (rotate(a,2)^rotate(a,13)^rotate(a,22)) + ((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=v+t; v=c; c=b; b=a; a=t+u;
    }
    d->h[0]+=a; d->h[1]+=b; d->h[2]+=c; d->h[3]+=v; d->h[4]+=e; d->h[5]+=f; d->h[6]+=g; d->h[7]+=h;
}
static Digest digest(void) {
    return (Digest){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
}
static void update(Digest *d, const void *data, size_t size) {
    const unsigned char *p = data; d->bytes += size;
    while (size) {
        size_t n = 64 - d->used; if (n > size) n = size;
        memcpy(d->block + d->used, p, n); d->used += n; p += n; size -= n;
        if (d->used == 64) { compress(d); d->used = 0; }
    }
}
static void finish(Digest d, char out[65]) {
    uint64_t bits = d.bytes * 8;
    unsigned char padding[128] = {0x80};
    size_t n = d.used < 56 ? 56 - d.used : 120 - d.used;
    update(&d, padding, n);
    for (unsigned i = 0; i < 8; i++) padding[i] = (unsigned char)(bits >> (56 - 8*i));
    update(&d, padding, 8);
    for (unsigned i = 0; i < 8; i++) snprintf(out + 8*i, 9, "%08x", d.h[i]);
}
void buildHashBytes(const void *data, size_t size, char hash[65]) {
    Digest d = digest(); update(&d, data, size); finish(d, hash);
}
static void number(Digest *d, uint64_t n) {
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; i++) bytes[i] = (unsigned char)(n >> (56-8*i));
    update(d, bytes, 8);
}
static void string(Digest *d, const char *s) {
    size_t n = s ? strlen(s) : 0; number(d, n); if (n) update(d, s, n);
}
static int hashFile(Digest *d, const char *path) {
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    unsigned char buffer[32768]; size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file))) update(d, buffer, n);
    int ok = !ferror(file); if (fclose(file)) ok = 0; return ok;
}
static char *join(const char *a, const char *b) {
    char *p = malloc(strlen(a) + strlen(b) + 2);
    if (p) sprintf(p, "%s/%s", a, b); return p;
}
static int directories(const char *path) {
    char *copy = strdup(path); if (!copy) return 0;
    int ok = 1;
    for (char *p = copy + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        struct stat info;
        if (mkdir(copy, 0700) && (errno != EEXIST || stat(copy, &info) || !S_ISDIR(info.st_mode))) ok = 0;
        *p = saved;
        if (!ok || !saved) break;
    }
    free(copy); return ok;
}
static char *cacheDirectory(void) {
    const char *override = getenv("OCEAN_BUILD_CACHE");
    if (override && !strcmp(override, "off")) return NULL;
    if (override && *override) return join(override, "v1");
    const char *home = getenv("HOME");
    if (!home || !*home) { struct passwd *user = getpwuid(getuid()); home = user ? user->pw_dir : NULL; }
    return home && *home ? join(home, ".cache/ocean-build/v1") : NULL;
}
static char *executable(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) ? NULL : realpath(name, NULL);
    const char *path = getenv("PATH"); if (!path) path = "/usr/bin:/bin";
    const char *start = path;
    do {
        const char *end = strchr(start, ':'); size_t n = end ? (size_t)(end-start) : strlen(start);
        char *directory = strndup(start, n), *candidate = directory ? join(n ? directory : ".", name) : NULL;
        free(directory);
        if (candidate && !access(candidate, X_OK)) {
            struct stat info;
            char *resolved = !stat(candidate, &info) && S_ISREG(info.st_mode) ? realpath(candidate, NULL) : NULL;
            free(candidate); if (resolved) return resolved;
        } else free(candidate);
        if (!end) break;
        start = end + 1;
    } while (1);
    return NULL;
}
struct buildCache { char *directory; char irKey[65], compilerHash[65]; };
buildCache *openBuildCache(const char *compiler, parseResult *program, const char *packageDirectory) {
    char *directory = cacheDirectory(); if (!directory) return NULL;
    char *self = executable(compiler), *cwd = getcwd(NULL, 0);
    Digest d = digest(); string(&d, "ocean merged IR action v1");
    Digest compilerDigest = digest();
    int compilerOK;
#ifdef __linux__
    // Hash the running image, including when an installation replaces its path.
    compilerOK = hashFile(&compilerDigest, "/proc/self/exe");
    if (!compilerOK) { compilerDigest = digest(); compilerOK = self && hashFile(&compilerDigest, self); }
#else
    compilerOK = self && hashFile(&compilerDigest, self);
#endif
    int ok = compilerOK && cwd;
    char compilerHash[65]; finish(compilerDigest, compilerHash); string(&d, compilerHash);
    free(self);
    string(&d, cwd); free(cwd); string(&d, packageDirectory);
    struct utsname host;
    if (uname(&host)) ok = 0;
    else { string(&d, host.sysname); string(&d, host.machine); }
    number(&d, sizeof(void *));
    string(&d, program->packageName); number(&d, program->isLibrary); number(&d, program->nSources);
    for (size_t i = 0; i < program->nSources; i++) {
        string(&d, program->sources[i].filename); number(&d, program->sources[i].packageId);
        string(&d, program->sources[i].text);
    }
    for (size_t i = 0; i < program->nStatements; i++)
        if (program->statements[i].type == STMT_FUNC)
            number(&d, ((struct stmt_func *)program->statements[i].value)->compilerIntrinsic);
    if (!ok || !directories(directory)) { free(directory); return NULL; }
    buildCache *cache = malloc(sizeof(*cache));
    if (!cache) { free(directory); return NULL; }
    cache->directory = directory; memcpy(cache->compilerHash, compilerHash, sizeof(compilerHash));
    finish(d, cache->irKey); return cache;
}
void closeBuildCache(buildCache *cache) { if (cache) { free(cache->directory); free(cache); } }
const char *buildCacheIRKey(buildCache *cache) { return cache ? cache->irKey : NULL; }
extern char **environ;
static int compareStrings(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void dependencySources(Digest *d, parseResult *program, size_t packageId, unsigned char *seen) {
    for (size_t i = 0; i < program->nPackages; i++) {
        packageInfo *p = &program->packages[i];
        if (p->id != packageId || seen[i]) continue;
        seen[i] = 1;
        string(d, p->directory);
        for (size_t j = 0; j < program->nSources; j++)
            if (program->sources[j].packageId == packageId) {
                string(d, program->sources[j].filename); string(d, program->sources[j].text);
            }
        for (size_t j = 0; j < p->nDependencies; j++)
            dependencySources(d, program, p->dependencies[j], seen);
    }
}
int buildCachePackageKey(buildCache *cache, parseResult *program, size_t packageId, int metadata, char key[65]) {
    if (!cache) return 0;
    char *tool = executable("clang"); struct stat info;
    if (!tool || stat(tool, &info)) { free(tool); return 0; }
    Digest d = digest();
    string(&d, "ocean source package action v3: native ABI v1; function/data sections");
    string(&d, cache->compilerHash); string(&d, tool); free(tool);
    number(&d, info.st_dev); number(&d, info.st_ino); number(&d, info.st_size); number(&d, info.st_mode);
#ifdef __APPLE__
    number(&d, info.st_mtimespec.tv_sec); number(&d, info.st_mtimespec.tv_nsec);
    number(&d, info.st_ctimespec.tv_sec); number(&d, info.st_ctimespec.tv_nsec);
#else
    number(&d, info.st_mtim.tv_sec); number(&d, info.st_mtim.tv_nsec);
    number(&d, info.st_ctim.tv_sec); number(&d, info.st_ctim.tv_nsec);
#endif
    struct utsname host;
    if (uname(&host)) return 0;
    string(&d, host.sysname); string(&d, host.release); string(&d, host.machine);
    char *cwd = getcwd(NULL, 0); if (!cwd) return 0;
    string(&d, cwd); free(cwd);
    number(&d, metadata);
    if (!packageId) string(&d, cache->irKey); // Complete source graph for shared support.
    else {
        unsigned char *seen = calloc(program->nPackages + 1, 1);
        if (!seen) return 0;
        dependencySources(&d, program, packageId, seen); free(seen);
        size_t root = 0;
        for (size_t i = 0; i < program->nSources; i++)
            if (!strcmp(program->sourceFilename, program->sources[i].filename)) root = program->sources[i].packageId;
        number(&d, root == packageId); number(&d, root == packageId && program->isLibrary);
        // Whether trusted intrinsics are enabled is an input as well as source.
        for (size_t i = 0; i < program->nStatements; i++) {
            stmt *s = &program->statements[i];
            if (s->type != STMT_FUNC) continue;
            struct stmt_func *f = s->value;
            if (!f->compilerIntrinsic) continue;
            for (size_t j = 0; j < program->nSources; j++)
                if (f->name.filename && !strcmp(f->name.filename, program->sources[j].filename) &&
                    program->sources[j].packageId == packageId) number(&d, f->compilerIntrinsic);
        }
    }
    size_t count = 0; while (environ[count]) count++;
    char **environment = malloc((count + 1) * sizeof(*environment)); if (!environment) return 0;
    memcpy(environment, environ, count * sizeof(*environment)); qsort(environment, count, sizeof(*environment), compareStrings);
    for (size_t i = 0; i < count; i++) string(&d, environment[i]);
    free(environment); finish(d, key); return 1;
}
static int validKey(const char *key) {
    if (!key || strlen(key) != 64) return 0;
    for (size_t i = 0; i < 64; i++) if (!strchr("0123456789abcdef", key[i])) return 0;
    return 1;
}
static char *entry(buildCache *cache, const char *kind, const char *key, int create) {
    char shard[3] = {key[0], key[1], 0};
    char *base = join(cache->directory, kind), *directory = base ? join(base, shard) : NULL;
    free(base);
    char *path = directory && (!create || directories(directory)) ? join(directory, key) : NULL;
    free(directory); return path;
}
/* The destination appears only after a complete, integrity-checked copy. */
static int copyFile(const char *source, const char *destination, const char *expected) {
    char *staged = malloc(strlen(destination) + 24); if (!staged) return 0;
    sprintf(staged, "%s.cache-XXXXXX", destination);
    int fd = mkstemp(staged), ok = 0, corrupt = 0;
    FILE *in = fopen(source, "rb"), *out = fd < 0 ? NULL : fdopen(fd, "wb");
    if (in && out) {
        Digest d = digest(); unsigned char buffer[32768]; size_t n; ok = 1;
        while ((n = fread(buffer, 1, sizeof(buffer), in))) {
            update(&d, buffer, n);
            if (fwrite(buffer, 1, n, out) != n) { ok = 0; break; }
        }
        if (ferror(in)) ok = 0;
        char actual[65]; finish(d, actual);
        if (expected && strcmp(actual, expected)) { ok = 0; corrupt = 1; }
    }
    if (in) fclose(in);
    if (out) { if (fclose(out)) ok = 0; } else if (fd >= 0) close(fd);
    if (ok && rename(staged, destination)) ok = 0;
    unlink(staged); free(staged); return ok ? 1 : corrupt ? -1 : 0;
}
int readBuildCache(buildCache *cache, const char *key, const char *destination) {
    if (!cache || !validKey(key)) return 0;
    char *manifest = entry(cache, "actions", key, 0); if (!manifest) return 0;
    FILE *file = fopen(manifest, "rb"); free(manifest); if (!file) return 0;
    char sum[66] = {0};
    int ok = fread(sum, 1, 65, file) == 65 && sum[64] == '\n' && fgetc(file) == EOF && !ferror(file);
    fclose(file); sum[64] = 0;
    if (!ok || !validKey(sum)) return 0;
    char *data = entry(cache, "data", sum, 0);
    int copied = data ? copyFile(data, destination, sum) : 0;
    if (copied < 0) unlink(data);
    free(data); return copied == 1;
}
void storeBuildCache(buildCache *cache, const char *key, const char *source) {
    if (!cache || !validKey(key)) return;
    Digest d = digest(); if (!hashFile(&d, source)) return;
    char sum[65]; finish(d, sum);
    char *data = entry(cache, "data", sum, 1), *manifest = entry(cache, "actions", key, 1);
    if (!data || !manifest || copyFile(source, data, sum) != 1) { free(data); free(manifest); return; }
    free(data);
    char *staged = malloc(strlen(manifest) + 24);
    if (staged) {
        sprintf(staged, "%s.cache-XXXXXX", manifest);
        int fd = mkstemp(staged); FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
        if (file) {
            int ok = fprintf(file, "%s\n", sum) == 65;
            if (fclose(file)) ok = 0;
            if (ok) rename(staged, manifest);
        } else if (fd >= 0) close(fd);
        unlink(staged); free(staged);
    }
    free(manifest);
}
/* Walk directory descriptors, never symlink targets, even during a race. */
static int removeAt(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        if (errno == ENOTDIR || errno == ELOOP)
            return unlinkat(parent, name, 0) && errno != ENOENT;
        return 1;
    }
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return 1; }
    int failed = 0; struct dirent *item;
    while ((item = readdir(directory))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (removeAt(dirfd(directory), item->d_name)) failed = 1;
    }
    closedir(directory);
    if (unlinkat(parent, name, AT_REMOVEDIR) && errno != ENOENT) failed = 1;
    return failed;
}
int cleanBuildCache(void) {
    char *directory = cacheDirectory(); if (!directory) return 0;
    int failed = removeAt(AT_FDCWD, directory); free(directory);
    if (failed) fprintf(stderr, "ocean: could not clean build cache\n");
    return failed;
}
