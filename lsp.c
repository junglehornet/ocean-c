#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "lsp.h"
#include "package.h"
#include "semantic.h"
#include "scanner.h"
#include "source_path.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

/* A small JSON reader. Each container owns a linked list of its values (object
 * members alternate keys and values). Raw spans preserve JSON-RPC request IDs. */
typedef struct Json {
    char kind, *text;
    const char *start, *end;
    struct Json *child, *next;
} Json;
static void freeJson(Json *j) {
    while (j) { Json *next = j->next; freeJson(j->child); free(j->text); free(j); j = next; }
}
static void whitespace(const char **p) { while (isspace((unsigned char)**p)) ++*p; }
static int hex4(const char **p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = *(*p)++;
        if (!isxdigit(c)) return -1;
        v = v * 16 + (c <= '9' ? c - '0' : tolower(c) - 'a' + 10);
    }
    return v;
}
static Json *readJson(const char **p, int depth) {
    whitespace(p);
    if (depth > 64 || !**p) return NULL;
    Json *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->start = *p; j->kind = **p;
    if (**p == '"') {
        ++*p;
        char *out = j->text = malloc(strlen(*p) + 1);
        if (!out) goto invalid;
        while (**p && **p != '"') {
            unsigned char c = *(*p)++;
            if (c < 32) goto invalid;
            if (c != '\\') { *out++ = c; continue; }
            c = *(*p)++;
            const char *esc = "\"\\/bfnrt", *decoded = "\"\\/\b\f\n\r\t";
            const char *found = c ? strchr(esc, c) : NULL;
            if (found) { *out++ = decoded[found - esc]; continue; }
            if (c != 'u' || strlen(*p) < 4) goto invalid;
            int code = hex4(p);
            if (code < 0 || code == 0) goto invalid;
            if (code >= 0xd800 && code <= 0xdbff) {
                if (strlen(*p) < 6 || (*p)[0] != '\\' || (*p)[1] != 'u') goto invalid;
                *p += 2; int low = hex4(p);
                if (low < 0xdc00 || low > 0xdfff) goto invalid;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) goto invalid;
            if (code < 0x80) *out++ = code;
            else if (code < 0x800) { *out++ = 0xc0 | (code >> 6); *out++ = 0x80 | (code & 63); }
            else if (code < 0x10000) {
                *out++ = 0xe0 | (code >> 12); *out++ = 0x80 | ((code >> 6) & 63); *out++ = 0x80 | (code & 63);
            } else {
                *out++ = 0xf0 | (code >> 18); *out++ = 0x80 | ((code >> 12) & 63);
                *out++ = 0x80 | ((code >> 6) & 63); *out++ = 0x80 | (code & 63);
            }
        }
        if (**p != '"') goto invalid;
        ++*p; *out = 0;
    } else if (**p == '{' || **p == '[') {
        char close = *(*p)++ == '{' ? '}' : ']';
        Json **tail = &j->child;
        whitespace(p);
        if (**p != close) for (;;) {
            Json *value = readJson(p, depth + 1);
            if (!value) goto invalid;
            *tail = value; tail = &value->next;
            if (j->kind == '{') {
                whitespace(p);
                if (value->kind != '"' || **p != ':') goto invalid;
                ++*p; value = readJson(p, depth + 1);
                if (!value) goto invalid;
                *tail = value; tail = &value->next;
            }
            whitespace(p);
            if (**p != ',') break;
            ++*p;
        }
        if (**p != close) goto invalid;
        ++*p;
    } else {
        const char *begin = *p;
        if (!strncmp(*p, "null", 4) || !strncmp(*p, "true", 4)) *p += 4;
        else if (!strncmp(*p, "false", 5)) *p += 5;
        else {
            if (**p == '-') ++*p;
            if (!isdigit((unsigned char)**p)) goto invalid;
            if (**p == '0') ++*p;
            else while (isdigit((unsigned char)**p)) ++*p;
            if (**p == '.') {
                ++*p; if (!isdigit((unsigned char)**p)) goto invalid;
                while (isdigit((unsigned char)**p)) ++*p;
            }
            if (**p == 'e' || **p == 'E') {
                ++*p; if (**p == '+' || **p == '-') ++*p;
                if (!isdigit((unsigned char)**p)) goto invalid;
                while (isdigit((unsigned char)**p)) ++*p;
            }
        }
        j->text = strndup(begin, (size_t)(*p - begin));
    }
    j->end = *p; return j;
invalid:
    freeJson(j); return NULL;
}
static Json *member(Json *j, const char *key) {
    if (!j || j->kind != '{') return NULL;
    for (Json *k = j->child; k && k->next; k = k->next->next)
        if (!strcmp(k->text, key)) return k->next;
    return NULL;
}
static const char *string(Json *j) { return j && j->kind == '"' ? j->text : NULL; }
static long integer(Json *j, long fallback) {
    if (!j || (j->kind != '-' && (j->kind < '0' || j->kind > '9')) || !j->text) return fallback;
    char *end; errno = 0; long value = strtol(j->text, &end, 10);
    return errno || *end ? fallback : value;
}
static void quoted(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 32) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
static void sendMessage(const char *body) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(body), body); fflush(stdout);
}
static void reply(Json *id, const char *result, int code, const char *message) {
    char *body = NULL; size_t length = 0;
    FILE *out = open_memstream(&body, &length);
    if (!out) return;
    fputs("{\"jsonrpc\":\"2.0\",\"id\":", out);
    if (id) fwrite(id->start, 1, (size_t)(id->end - id->start), out); else fputs("null", out);
    if (code) { fprintf(out, ",\"error\":{\"code\":%d,\"message\":", code); quoted(out, message); fputs("}}", out); }
    else fprintf(out, ",\"result\":%s}", result ? result : "null");
    fclose(out); sendMessage(body); free(body);
}

typedef struct Document {
    char *uri, *path, *text;
    long version;
    struct Document *next;
} Document;
static char *uriPath(const char *uri) {
    if (strncmp(uri, "file://", 7)) return NULL;
    const char *s = uri + 7;
    if (!strncmp(s, "localhost/", 10)) s += 9;
    if (*s != '/') return NULL;
    char *path = malloc(strlen(s) + 1), *out = path;
    if (!path) return NULL;
    while (*s) {
        if (*s == '%') {
            if (!s[1] || !s[2] || !isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2])) { free(path); return NULL; }
            char hex[] = {s[1], s[2], 0}; int c = (int)strtol(hex, NULL, 16);
            if (!c) { free(path); return NULL; }
            *out++ = c; s += 3;
        } else *out++ = *s++;
    }
    *out = 0;
#ifdef _WIN32
    if (path[0] == '/' && isalpha((unsigned char)path[1]) && path[2] == ':') memmove(path, path + 1, strlen(path));
#endif
    char *resolved = realpath(path, NULL);
    if (!resolved) { /* New files can still have a real parent directory. */
        char *slash = strrchr(path, '/');
        if (slash && slash != path) {
            *slash = 0; char *parent = realpath(path, NULL); *slash = '/';
            if (parent) {
                resolved = malloc(strlen(parent) + strlen(slash) + 1);
                if (resolved) { strcpy(resolved, parent); strcat(resolved, slash); }
                free(parent);
            }
        }
    }
    free(path); return resolved;
}
static long utf16Column(const char *text, long line, long column) {
    while (line > 1 && *text) { if (*text++ == '\n') line--; }
    long units = 0;
    for (long i = 0; i < column && text[i] && text[i] != '\n' && text[i] != '\r'; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
    }
    return units;
}
#ifdef _WIN32
static int windowsLspRequest(FILE *output, const char *method, Json *params, Document *doc, Document *documents);
#endif
static void publish(Document *doc, Document *documents, int clear) {
    char *body = NULL; size_t length = 0;
    FILE *out = open_memstream(&body, &length);
    if (!out) return;
    fputs("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":", out);
    quoted(out, doc->uri); fprintf(out, ",\"version\":%ld,\"diagnostics\":[", doc->version);
    FILE *errors = clear ? NULL : tmpfile();
    if (errors) {
#ifdef _WIN32
        if (!windowsLspRequest(errors, "diagnostics", NULL, doc, documents)) {
#else
        fflush(NULL);
        pid_t child = fork();
        if (!child) {
            dup2(fileno(errors), STDERR_FILENO);
            FILE *sink = freopen("/dev/null", "w", stdout); (void)sink;
            setenv("NO_COLOR", "1", 1);
            size_t count = 0;
            for (Document *d = documents; d; d = d->next) count++;
            packageSource *sources = calloc(count, sizeof(*sources));
            if (!sources) _exit(1);
            size_t i = 0;
            for (Document *d = documents; d; d = d->next) sources[i++] = (packageSource){d->path, d->text};
            char *directory = strdup(doc->path), *slash = strrchr(directory, '/');
            if (slash == directory) slash[1] = 0; else *slash = 0;
            packageBuild *build = loadEditorPackage(directory, sources, count);
            parseResult *program = packagePartialProgram(build);
            if (program) analyzeSemantics(program);
            fflush(stderr); _exit(0); /* Also releases parser allocations on scanner failure. */
        }
        if (child > 0) {
            int status;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
#endif
            rewind(errors);
            char *line = NULL; size_t capacity = 0; int comma = 0;
            char *display = relativeSourcePath(doc->path);
            while (getline(&line, &capacity, errors) >= 0) {
                if (line[0] != '[') continue;
                char *end = strstr(line, "]: ");
                if (!end) continue;
                *end = 0;
                char *col = strrchr(line, ':'); if (!col) continue; *col++ = 0;
                char *row = strrchr(line, ':'); if (!row) continue; *row++ = 0;
                if (!display || strcmp(line + 1, display)) continue;
                long n = strtol(row, NULL, 10), c = strtol(col, NULL, 10);
                if (n < 1 || c < 1) continue;
                char *message = end + 3; int severity = strstr(message, " warning: ") ? 2 : 1;
                message[strcspn(message, "\r\n")] = 0;
                c = utf16Column(doc->text, n, c - 1);
                if (comma++) fputc(',', out);
                fprintf(out, "{\"range\":{\"start\":{\"line\":%ld,\"character\":%ld},\"end\":{\"line\":%ld,\"character\":%ld}},\"severity\":%d,\"source\":\"ocean\",\"message\":", n-1,c,n-1,c,severity);
                quoted(out, message); fputc('}', out);
            }
            free(display); free(line);
        }
        fclose(errors);
    }
    fputs("]}}", out); fclose(out); sendMessage(body); free(body);
}
#include "lsp_semantic.inc"
#ifdef _WIN32
/* Native Windows cannot fork the editor's in-memory overlays. Serialize them
 * into a private request and re-exec an isolated compiler worker instead. */
static int windowsLspRequest(FILE *output, const char *method, Json *params, Document *doc, Document *documents) {
    char request[4096], result[4096];
    oceanTemporary(request, sizeof(request), "ocean-lsp-request-XXXXXX");
    oceanTemporary(result, sizeof(result), "ocean-lsp-result-XXXXXX");
    int in = mkstemp(request), out = mkstemp(result);
    if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); unlink(request); unlink(result); return 1; }
    close(out); FILE *file = fdopen(in, "wb");
    if (!file) { close(in); unlink(request); unlink(result); return 1; }
    fputs("{\"method\":", file); quoted(file, method);
    fputs(",\"selected\":", file); quoted(file, doc->uri);
    fputs(",\"params\":", file);
    if (params) fwrite(params->start, 1, (size_t)(params->end - params->start), file); else fputs("null", file);
    fputs(",\"documents\":[", file); int comma = 0;
    for (Document *d = documents; d; d = d->next) {
        if (comma++) fputc(',', file);
        fputs("{\"uri\":", file); quoted(file, d->uri);
        fputs(",\"path\":", file); quoted(file, d->path);
        fputs(",\"text\":", file); quoted(file, d->text); fputc('}', file);
    }
    fputs("]}", file); int failed = fclose(file) != 0;
    char *executable = oceanExecutable();
    char *args[] = {executable, "--internal-lsp-worker", request, result, NULL};
    if (!executable || (!failed && oceanRun(args))) failed = 1;
    free(executable);
    FILE *read = failed ? NULL : fopen(result, "rb");
    if (!read) failed = 1;
    else { char bytes[8192]; size_t n; while ((n = fread(bytes, 1, sizeof(bytes), read))) if (fwrite(bytes, 1, n, output) != n) failed = 1; fclose(read); }
    fflush(output); unlink(request); unlink(result); return failed;
}
int oceanLspWorker(const char *request, const char *result) {
    FILE *file = fopen(request, "rb"); if (!file) return 1;
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    char *text = length >= 0 ? calloc((size_t)length + 1, 1) : NULL;
    if (!text) { fclose(file); return 1; }
    fread(text, 1, (size_t)length, file); fclose(file);
    const char *cursor = text; Json *json = readJson(&cursor, 0); if (!json) { free(text); return 1; }
    Document *documents = NULL, *selected = NULL;
    const char *uri = string(member(json, "selected"));
    Json *array = member(json, "documents");
    for (Json *item = array ? array->child : NULL; item; item = item->next) {
        Document *d = calloc(1, sizeof(*d)); if (!d) return 1;
        d->uri = (char *)string(member(item, "uri")); d->path = (char *)string(member(item, "path")); d->text = (char *)string(member(item, "text"));
        if (!d->uri || !d->path || !d->text) return 1;
        d->next = documents; documents = d;
        if (uri && !strcmp(d->uri, uri)) selected = d;
    }
    if (!selected) return 1;
    const char *method = string(member(json, "method"));
    FILE *sink = freopen("NUL", "wb", stdout); (void)sink;
    if (method && !strcmp(method, "diagnostics")) {
        sink = freopen(result, "wb", stderr); if (!sink) return 1;
        setenv("NO_COLOR", "1", 1); size_t count = 0; for (Document *d = documents; d; d = d->next) count++;
        packageSource *sources = calloc(count, sizeof(*sources)); if (!sources) return 1;
        size_t i = 0; for (Document *d = documents; d; d = d->next) sources[i++] = (packageSource){d->path, d->text};
        char *directory = strdup(selected->path), *slash = strrchr(directory, '/'); if (!slash) return 1; *slash = 0;
        packageBuild *build = loadEditorPackage(directory, sources, count); parseResult *program = packagePartialProgram(build);
        if (program) analyzeSemantics(program); fflush(stderr);
    } else {
        sink = freopen("NUL", "wb", stderr); (void)sink;
        FILE *out = fopen(result, "wb"); if (!out || !method) return 1;
        semanticWorker(out, method, member(json, "params"), selected, documents); fclose(out);
    }
    return 0;
}
#endif
static void freeDocument(Document *d) { free(d->uri); free(d->path); free(d->text); free(d); }

int oceanLsp(void) {
    Document *documents = NULL;
    int initialized = 0, shutdown = 0, result = 1;
    for (;;) {
        char header[1024]; size_t length = 0; int haveLength = 0, bad = 0;
        while (fgets(header, sizeof(header), stdin)) {
            if (!strcmp(header, "\r\n") || !strcmp(header, "\n")) break;
            if (!strchr(header, '\n')) { bad = 1; break; }
            if (!strncasecmp(header, "Content-Length:", 15)) {
                char *end; errno = 0; unsigned long long value = strtoull(header + 15, &end, 10);
                while (isspace((unsigned char)*end)) end++;
                if (errno || *end || haveLength || value > 16 * 1024 * 1024 || !value) { bad = 1; break; }
                length = (size_t)value; haveLength = 1;
            }
        }
        if (feof(stdin)) { result = shutdown ? 0 : 1; break; }
        if (bad || !haveLength) break;
        char *body = calloc(length + 1, 1);
        if (!body) break;
        if (fread(body, 1, length, stdin) != length) { free(body); break; }
        const char *cursor = body;
        Json *root = readJson(&cursor, 0); whitespace(&cursor);
        if (!root || *cursor) { reply(NULL, NULL, -32700, "Parse error"); freeJson(root); free(body); continue; }
        Json *id = member(root, "id"), *params = member(root, "params");
        const char *method = string(member(root, "method"));
        if (!method) { if (id || root->kind != '{') reply(id, NULL, -32600, "Invalid request"); }
        else if (!strcmp(method, "exit")) { result = shutdown ? 0 : 1; freeJson(root); free(body); break; }
        else if (shutdown) { if (id) reply(id, NULL, -32600, "Server has shut down"); }
        else if (!strcmp(method, "initialize")) {
            if (initialized) reply(id, NULL, -32600, "Already initialized");
            else if (id) {
                initialized = 1;
                reply(id, "{\"capabilities\":{\"positionEncoding\":\"utf-16\",\"hoverProvider\":true,\"definitionProvider\":true,\"declarationProvider\":true,\"typeDefinitionProvider\":true,\"referencesProvider\":true,\"renameProvider\":{\"prepareProvider\":true},\"completionProvider\":{\"triggerCharacters\":[\".\"]},\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":true}},\"serverInfo\":{\"name\":\"ocean\"}}", 0, NULL);
            }
        } else if (!initialized) { if (id) reply(id, NULL, -32002, "Server not initialized"); }
        else if (!strcmp(method, "shutdown")) { if (id) { shutdown = 1; reply(id, "null", 0, NULL); } }
        else if (!strcmp(method,"textDocument/hover")||!strcmp(method,"textDocument/definition")||!strcmp(method,"textDocument/declaration")||!strcmp(method,"textDocument/typeDefinition")||!strcmp(method,"textDocument/references")||!strcmp(method,"textDocument/rename")||!strcmp(method,"textDocument/prepareRename")||!strcmp(method,"textDocument/completion")) {
            if(id)semanticRequest(id,method,params,documents);
        }
        else if (!strcmp(method, "textDocument/didOpen") || !strcmp(method, "textDocument/didChange") ||
                 !strcmp(method, "textDocument/didSave") || !strcmp(method, "textDocument/didClose")) {
            Json *td = member(params, "textDocument");
            const char *uri = string(member(td, "uri"));
            if (uri) {
                Document **slot = &documents;
                while (*slot && strcmp((*slot)->uri, uri)) slot = &(*slot)->next;
                Document *d = *slot;
                Json *version = member(td, "version");
                if (!strcmp(method, "textDocument/didOpen")) {
                    const char *text = string(member(td, "text")); char *path = uriPath(uri);
                    if (text && path) {
                        if (!d) { d = calloc(1, sizeof(*d)); d->uri = strdup(uri); *slot = d; }
                        free(d->path); free(d->text); d->path = path; d->text = strdup(text);
                        d->version = integer(version, 0);
                    } else free(path);
                } else if (d && !strcmp(method, "textDocument/didClose")) {
                    publish(d, documents, 1); *slot = d->next; freeDocument(d); d = NULL;
                } else if (d && !strcmp(method, "textDocument/didChange")) {
                    Json *changes = member(params, "contentChanges");
                    long v = integer(version, d->version + 1);
                    if (changes && changes->kind == '[' && v > d->version) {
                        for (Json *change = changes->child; change; change = change->next) {
                            const char *text = string(member(change, "text"));
                            if (text && !member(change, "range")) { free(d->text); d->text = strdup(text); }
                        }
                        d->version = v;
                    }
                }
                /* Recheck all open documents so edits to sibling files propagate. */
                for (Document *open = documents; open; open = open->next) publish(open, documents, 0);
            }
        } else if (id) reply(id, NULL, -32601, "Method not found");
        freeJson(root); free(body);
    }
    while (documents) { Document *next = documents->next; freeDocument(documents); documents = next; }
    return result;
}
