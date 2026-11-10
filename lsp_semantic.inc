static int sameToken(token a, token b) {
    return a.filename && b.filename && !strcmp(a.filename, b.filename) && a.line == b.line &&
           a.column == b.column;
}
/* Included by lsp.c: semantic requests run in an isolated worker, like diagnostics. */
static size_t bytePosition(const char *s, long line, long character) {
    size_t p = 0;
    while (s[p] && line > 0)
        if (s[p++] == '\n')
            line--;
    while (s[p] && s[p] != '\n' && character > 0) {
        uint32_t c = 0;
        size_t n = unicodeDecode(s + p, &c);
        if (!n)
            n = 1;
        long width = c >= 0x10000 ? 2 : 1;
        if (character < width)
            break;
        character -= width;
        p += n;
    }
    return p;
}
static size_t tokenPosition(const char *s, token t) {
    size_t p = 0;
    for (long line = 1; s[p] && line < t.line;)
        if (s[p++] == '\n')
            line++;
    return p + (t.column > 0 ? t.column - 1 : 0);
}
static size_t wordLength(const char *s) {
    size_t n = 0, k;
    uint32_t c = 0;
    while ((k = unicodeDecode(s + n, &c)) && unicodeContinue(c))
        n += k;
    return n;
}
static void wordAt(const char *s, size_t at, size_t *begin, size_t *end) {
    size_t p = 0;
    *begin = *end = at;
    while (s[p] && p <= at) {
        uint32_t c = 0;
        size_t n = unicodeDecode(s + p, &c);
        if (!n)
            n = 1;
        if (unicodeStart(c)) {
            size_t length = wordLength(s + p);
            if (at >= p && at <= p + length) {
                *begin = p;
                *end = p + length;
                return;
            }
            p += length;
        } else
            p += n;
    }
}
static const char *fileText(parseResult *program, const char *filename) {
    return programSource(program, filename);
}
static void positionJSON(FILE *out, const char *s, size_t offset) {
    size_t line = 0, start = 0;
    for (size_t i = 0; s[i] && i < offset; i++)
        if (s[i] == '\n') {
            line++;
            start = i + 1;
        }
    fprintf(out, "{\"line\":%zu,\"character\":%ld}", line,
            utf16Column(s + start, 1, offset - start));
}
static void rangeJSON(FILE *out, const char *text, token t) {
    size_t p = tokenPosition(text, t);
    fputs("{\"start\":", out);
    positionJSON(out, text, p);
    fputs(",\"end\":", out);
    positionJSON(out, text, p + wordLength(text + p));
    fputc('}', out);
}
static char *fileURI(const char *filename) {
    char *absolute;
    if (filename[0] == '/')
        absolute = strdup(filename);
    else {
        char *cwd = getcwd(NULL, 0);
        absolute = malloc(strlen(cwd) + strlen(filename) + 2);
        sprintf(absolute, "%s/%s", cwd, filename);
        free(cwd);
    }
    char *directory = strdup(absolute), *slash = strrchr(directory, '/');
    char *name = strdup(slash ? slash + 1 : absolute);
    if (slash)
        *slash = 0;
    char *resolved = realpath(directory, NULL);
    if (resolved) {
        free(absolute);
        absolute = malloc(strlen(resolved) + strlen(name) + 2);
        sprintf(absolute, "%s/%s", resolved, name);
        free(resolved);
    }
    free(name);
    free(directory);
    char *uri = malloc(8 + 3 * strlen(absolute)), *p = uri;
    memcpy(p, "file://", 7);
    p += 7;
    for (unsigned char *s = (unsigned char *)absolute; *s; s++) {
        if (isalnum(*s) || strchr("/-._~", *s))
            *p++ = *s;
        else {
            sprintf(p, "%%%02X", *s);
            p += 3;
        }
    }
    *p = 0;
    free(absolute);
    return uri;
}
static void locationJSON(FILE *out, parseResult *program, token t) {
    char *uri = fileURI(t.filename);
    fputs("{\"uri\":", out);
    quoted(out, uri);
    free(uri);
    fputs(",\"range\":", out);
    rangeJSON(out, fileText(program, t.filename), t);
    fputc('}', out);
}
typedef struct {
    const char *filename, *alias;
    size_t packageId;
} LspImport;
typedef struct {
    LspImport *values;
    size_t count;
} LspImports;
static void collectImport(const char *filename, const char *alias, size_t packageId,
                          void *context) {
    LspImports *a = context;
    a->values = realloc(a->values, (a->count + 1) * sizeof(*a->values));
    a->values[a->count++] = (LspImport){filename, alias, packageId};
}
static int identifierName(const char *name) {
    if (!name || !strcmp(name, "_"))
        return 0;
    uint32_t c = 0;
    size_t p = unicodeDecode(name, &c);
    if (!p || !unicodeStart(c))
        return 0;
    while (name[p]) {
        size_t n = unicodeDecode(name + p, &c);
        if (!n || !unicodeContinue(c))
            return 0;
        p += n;
    }
    const char *reserved[] = {"asm", "type",      "struct",      "func",   "import", "any",  "mixed",
                              "interface", "continue",    "break",  "enum",   "if",   "defer",
                              "switch",    "fallthrough", "extern", "for",    "goto", "as",
                              "package",   "else",        "range",  "return", "case", "default",
                              "null",      "panic",       "true",   "false"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(*reserved); i++)
        if (!strcmp(name, reserved[i]))
            return 0;
    return 1;
}
static void completionItem(FILE *out, int *comma, const char *name, const char *detail, int kind) {
    if ((*comma)++)
        fputc(',', out);
    fputs("{\"label\":", out);
    quoted(out, name);
    fputs(",\"detail\":", out);
    quoted(out, detail);
    fprintf(out, ",\"kind\":%d}", kind);
}
static void semanticWorker(FILE *out, const char *method, Json *params, Document *doc,
                           Document *documents) {
    Json *position = member(params, "position");
    size_t cursor = bytePosition(doc->text, integer(member(position, "line"), 0),
                                 integer(member(position, "character"), 0));
    size_t begin, end;
    wordAt(doc->text, cursor, &begin, &end);
    int completion = !strcmp(method, "textDocument/completion"),
        memberMode = completion && begin > 0 && doc->text[begin - 1] == '.';
    char *edited = NULL;
    if (completion) {
        // Keep complete buffers unchanged. Repair only the completion expression,
        // without writing editor contents or substituting lexical name matching.
        size_t n = strlen(doc->text);
        edited = malloc(n + 48);
        memcpy(edited, doc->text, begin);
        edited[begin] = 0;
        const char *replacement =
            memberMode ? "oceanCompletionPlaceholder" : "oceanCompletionPlaceholder";
        strcat(edited, replacement);
        size_t next = end;
        while (doc->text[next] == ' ' || doc->text[next] == '\t')
            next++;
        if (!doc->text[next] || doc->text[next] == '\n' || doc->text[next] == '}')
            strcat(edited, ";");
        strcat(edited, doc->text + end);
    }
    size_t count = 0;
    for (Document *d = documents; d; d = d->next)
        count++;
    packageSource *overlays = calloc(count, sizeof(*overlays));
    size_t i = 0;
    for (Document *d = documents; d; d = d->next)
        overlays[i++] = (packageSource){d->path, d == doc && edited ? edited : d->text};
    char *directory = strdup(doc->path), *slash = strrchr(directory, '/');
    if (slash == directory)
        slash[1] = 0;
    else
        *slash = 0;
    packageBuild *build = loadPackageWithSources(directory, overlays, count);
    parseResult *program = packagePartialProgram(build);
    if (!program) {
        fputs(completion ? "{\"isIncomplete\":true,\"items\":[]}" : "null", out);
        free(directory);
        free(overlays);
        free(edited);
        freePackage(build);
        return;
    }
    semanticIndex index = {0};
    indexSemantics(program, &index);
    char *display = relativeSourcePath(doc->path);
    size_t packageId = 0;
    for (i = 0; i < program->nSources; i++)
        if (!strcmp(program->sources[i].filename, display))
            packageId = program->sources[i].packageId;
    size_t binding = (size_t)-1;
    semanticReference *reference = NULL;
    for (i = 0; i < index.nReferences; i++)
        if (!strcmp(index.references[i].use.filename, display)) {
            size_t p = tokenPosition(doc->text, index.references[i].use),
                   n = wordLength(doc->text + p);
            if (cursor >= p && cursor <= p + n) {
                binding = index.references[i].symbol;
                reference = &index.references[i];
                break;
            }
        }
    if (binding == (size_t)-1)
        for (i = 0; i < index.nSymbols; i++)
            if (!strcmp(index.symbols[i].declaration.filename, display)) {
                size_t p = tokenPosition(doc->text, index.symbols[i].declaration),
                       n = wordLength(doc->text + p);
                if (cursor >= p && cursor <= p + n) {
                    binding = i;
                    break;
                }
            }
    if (completion) {
        int comma = 0;
        fputs("{\"isIncomplete\":false,\"items\":[", out);
        char *prefix = strndup(doc->text + begin, cursor - begin);
        size_t prefixLength = strlen(prefix);
        if (memberMode) {
            size_t receiverBegin, receiverEnd;
            wordAt(doc->text, begin - 2, &receiverBegin, &receiverEnd);
            char *receiver = strndup(doc->text + receiverBegin, receiverEnd - receiverBegin);
            LspImports imports = {0};
            visitPackageImports(build, collectImport, &imports);
            size_t imported = 0;
            for (i = 0; i < imports.count; i++)
                if (!strcmp(imports.values[i].filename, display) &&
                    !strcmp(imports.values[i].alias, receiver))
                    imported = imports.values[i].packageId;
            if (imported) {
                for (i = 0; i < index.nSymbols; i++) {
                    semanticSymbol *s = &index.symbols[i];
                    if (s->global && !s->member && s->packageId == imported &&
                        exportedName(s->name) && !strncmp(s->name, prefix, prefixLength))
                        completionItem(out, &comma, s->name, s->type, s->kind);
                }
            } else
                for (i = index.nMembers; i-- > 0;) {
                    semanticMembers *group = &index.members[i];
                    if (strcmp(group->use.filename, display) ||
                        (tokenPosition(doc->text, group->use) != begin &&
                         tokenPosition(doc->text, group->use) != receiverBegin))
                        continue;
                    for (size_t j = 0; j < group->count; j++) {
                        semanticSymbol *s = &index.symbols[group->symbols[j]];
                        if (!strncmp(s->name, prefix, prefixLength))
                            completionItem(out, &comma, s->name, s->type, s->kind);
                    }
                    if (group->collection == 2) {
                        const char *names[] = {"size", "data"};
                        for (size_t j = 0; j < 2; j++)
                            if (!strncmp(names[j], prefix, prefixLength))
                                completionItem(out, &comma, names[j], "array field", 10);
                    } else if (group->collection) {
                        const char *names[] = {"length", "capacity", "size", "data",   "array",
                                               "append", "prepend",  "add",  "remove", "pop"};
                        for (size_t j = 0; j < (group->immutable ? 5 : 10); j++)
                            if (!strncmp(names[j], prefix, prefixLength))
                                completionItem(out, &comma, names[j], "dynamic array member",
                                               j < 4 ? 10 : 2);
                    }
                    break;
                }
            free(imports.values);
            free(receiver);
        } else {
            for (i = 0; i < index.nSymbols; i++) {
                semanticSymbol *s = &index.symbols[i];
                if (s->member || s->packageId != packageId ||
                    strncmp(s->name, prefix, prefixLength))
                    continue;
                if (!s->global && (strcmp(s->declaration.filename, display) || cursor < s->begin ||
                                   cursor > s->end ||
                                   (s->kind != 3 && s->kind != 7 &&
                                    tokenPosition(doc->text, s->declaration) > cursor)))
                    continue;
                int shadowed = 0;
                for (size_t j = 0; j < index.nSymbols; j++) {
                    semanticSymbol *other = &index.symbols[j];
                    if (j == i || other->member || other->global ||
                        strcmp(other->declaration.filename, display) ||
                        strcmp(other->name, s->name))
                        continue;
                    if (cursor >= other->begin && cursor <= other->end &&
                        tokenPosition(doc->text, other->declaration) <= cursor &&
                        (s->global || other->begin > s->begin))
                        shadowed = 1;
                }
                if (!shadowed)
                    completionItem(out, &comma, s->name, s->type, s->kind);
            }
            const char *builtins[] = {"uint", "u64", "u32", "u16", "u8", "int",    "i8",  "i16",  "i32",    "i64", "float",
                                      "f32",    "f64", "bool", "string", "any", "array", "Result",
                                      "buffer", "map", "delete", "len", "new", "free", "recover"};
            for (i = 0; i < sizeof(builtins) / sizeof(*builtins); i++)
                if (!strncmp(builtins[i], prefix, prefixLength))
                    completionItem(out, &comma, builtins[i], "builtin", 14);
        }
        fputs("]}", out);
        free(prefix);
    } else if (binding == (size_t)-1)
        fputs(!strcmp(method, "textDocument/references") ? "[]" : "null", out);
    else {
        semanticSymbol *s = &index.symbols[binding];
        if (!strcmp(method, "textDocument/hover")) {
            char *detail = NULL;
            size_t length = 0;
            FILE *text = open_memstream(&detail, &length);
            fprintf(text, "```ocean\n%s: %s\n```", s->name, reference ? reference->type : s->type);
            fclose(text);
            fputs("{\"contents\":{\"kind\":\"markdown\",\"value\":", out);
            quoted(out, detail);
            free(detail);
            fputs("},\"range\":", out);
            rangeJSON(out, doc->text, reference ? reference->use : s->declaration);
            fputc('}', out);
        } else if (!strcmp(method, "textDocument/definition") ||
                   !strcmp(method, "textDocument/declaration") ||
                   !strcmp(method, "textDocument/typeDefinition")) {
            fputc('[', out);
            token target = !strcmp(method, "textDocument/typeDefinition") ? s->typeDeclaration
                                                                          : s->declaration;
            if (target.filename && strcmp(target.filename, OCEAN_BUILTIN_RESULT_FILE))
                locationJSON(out, program, target);
            fputc(']', out);
        } else if (!strcmp(method, "textDocument/prepareRename") &&
                   s->declaration.filename && !strcmp(s->declaration.filename, OCEAN_BUILTIN_RESULT_FILE)) {
            fputs("null", out);
        } else if (!strcmp(method, "textDocument/prepareRename")) {
            fputs("{\"range\":", out);
            rangeJSON(out, doc->text, reference ? reference->use : s->declaration);
            fputs(",\"placeholder\":", out);
            quoted(out, s->name);
            fputc('}', out);
        } else if (!strcmp(method, "textDocument/references")) {
            int comma = 0;
            Json *include = member(member(params, "context"), "includeDeclaration");
            fputc('[', out);
            if (include && include->kind == 't' && s->declaration.filename &&
                strcmp(s->declaration.filename, OCEAN_BUILTIN_RESULT_FILE)) {
                locationJSON(out, program, s->declaration);
                comma = 1;
            }
            for (i = 0; i < index.nReferences; i++)
                if (index.references[i].symbol == binding &&
                    strcmp(index.references[i].use.filename, OCEAN_BUILTIN_RESULT_FILE)) {
                    if (comma++)
                        fputc(',', out);
                    locationJSON(out, program, index.references[i].use);
                }
            fputc(']', out);
        } else if (!strcmp(method, "textDocument/rename")) {
            const char *newName = string(member(params, "newName"));
            int conflict = !identifierName(newName) ||
                           (s->declaration.filename && !strcmp(s->declaration.filename, OCEAN_BUILTIN_RESULT_FILE));
            for (i = 0; !conflict && i < index.nSymbols; i++) {
                semanticSymbol *other = &index.symbols[i];
                if (i == binding || strcmp(other->name, newName) ||
                    other->packageId != s->packageId || other->member != s->member ||
                    (s->member && !sameToken(s->owner, other->owner)))
                    continue;
                if (s->global || other->global ||
                    (!strcmp(other->declaration.filename, s->declaration.filename) &&
                     other->begin <= s->end && s->begin <= other->end))
                    conflict = 1;
            }
            for (i = 0; !conflict && i < index.nReferences; i++)
                if (index.references[i].symbol == binding && !exportedName(newName))
                    for (size_t j = 0; j < program->nSources; j++)
                        if (!strcmp(program->sources[j].filename,
                                    index.references[i].use.filename) &&
                            program->sources[j].packageId != s->packageId)
                            conflict = 1;
            if (conflict)
                fputs("{\"oceanError\":\"Rename would create an invalid identifier, collision, or "
                      "inaccessible export\"}",
                      out);
            else {
                fputs("{\"changes\":{", out);
                int files = 0;
                for (size_t file = 0; file < program->nSources; file++) {
                    sourceFile *src = &program->sources[file];
                    int used = !strcmp(src->filename, s->declaration.filename);
                    for (i = 0; i < index.nReferences; i++)
                        if (index.references[i].symbol == binding &&
                            !strcmp(index.references[i].use.filename, src->filename))
                            used = 1;
                    if (!used)
                        continue;
                    if (files++)
                        fputc(',', out);
                    char *uri = fileURI(src->filename);
                    quoted(out, uri);
                    free(uri);
                    fputs(":[", out);
                    int comma = 0;
                    if (!strcmp(src->filename, s->declaration.filename)) {
                        fputs("{\"range\":", out);
                        rangeJSON(out, src->text, s->declaration);
                        fputs(",\"newText\":", out);
                        quoted(out, newName);
                        fputc('}', out);
                        comma = 1;
                    }
                    for (i = 0; i < index.nReferences; i++) {
                        semanticReference *r = &index.references[i];
                        if (r->symbol != binding || strcmp(r->use.filename, src->filename) ||
                            sameToken(r->use, s->declaration))
                            continue;
                        if (comma++)
                            fputc(',', out);
                        fputs("{\"range\":", out);
                        rangeJSON(out, src->text, r->use);
                        fputs(",\"newText\":", out);
                        quoted(out, newName);
                        fputc('}', out);
                    }
                    fputc(']', out);
                }
                fputs("}}", out);
            }
        } else
            fputs("null", out);
    }
    free(display);
    freeSemanticIndex(&index);
    freePackage(build);
    free(directory);
    free(overlays);
    free(edited);
}
static void semanticRequest(Json *id, const char *method, Json *params, Document *documents) {
    const char *uri = string(member(member(params, "textDocument"), "uri"));
    Document *doc = documents;
    while (doc && (!uri || strcmp(doc->uri, uri)))
        doc = doc->next;
    Document disk = {0};
    if (!doc && uri) {
        disk.path = uriPath(uri);
        if (disk.path) {
            FILE *file = fopen(disk.path, "rb");
            if (file) {
                fseek(file, 0, SEEK_END);
                long n = ftell(file);
                rewind(file);
                if (n >= 0) {
                    disk.text = calloc(n + 1, 1);
                    fread(disk.text, 1, n, file);
                }
                fclose(file);
            }
        }
        if (disk.text) {
            disk.uri = (char *)uri;
            disk.next = documents;
            doc = &disk;
            documents = doc;
        }
    }
    if (!doc) {
        reply(id, NULL, 0, NULL);
        free(disk.path);
        free(disk.text);
        return;
    }
    FILE *result = tmpfile();
    if (!result) {
        reply(id, NULL, -32603, "Could not create semantic worker");
        return;
    }
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        freopen("/dev/null", "w", stderr);
        freopen("/dev/null", "w", stdout);
        semanticWorker(result, method, params, doc, documents);
        fflush(result);
        _exit(0);
    }
    int status = 0;
    if (child > 0)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        reply(id, NULL, -32603, "Could not analyze document");
    else {
        long n = ftell(result);
        rewind(result);
        char *text = calloc(n + 1, 1);
        fread(text, 1, n, result);
        if (strstr(text, "\"oceanError\""))
            reply(id, NULL, -32602,
                  "Rename would create an invalid identifier, collision, or inaccessible export");
        else
            reply(id, *text ? text : "null", 0, NULL);
        free(text);
    }
    fclose(result);
    free(disk.path);
    free(disk.text);
}
