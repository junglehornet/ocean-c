/* Native package modules are written directly from backend-owned function
 * buffers. This is not an IR splitter: ordinary function ownership is known
 * before emission, and runtime declarations are collected in one linear pass.
 * Numeric compiler-local IDs are replaced with stable semantic symbol names at
 * serialization so an unchanged package can link against rebuilt support. */
static void nativeName(Backend *b, const char *from, const char *to) {
    if (!b->units) return;
    uintptr_t previous;
    if (hashmap_get(b->nativeNames, from, strlen(from), &previous)) return;
    NativeName *name = checked(sizeof(*name));
    *name = (NativeName){strdup(from), strdup(to), b->nativeNameStorage};
    b->nativeNameStorage = name;
    if (hashmap_set(b->nativeNames, name->from, strlen(name->from), (uintptr_t)name->to) < 0)
        fail(b, "could not register native symbol identity");
}
static const char *nativePackageName(Backend *b, const char *text) {
    const char *prefix = strstr(text, "__pkg");
    if (!prefix || !isdigit((unsigned char)prefix[5])) return text;
    char *end; unsigned long long id = strtoull(prefix + 5, &end, 10);
    if (*end != '_') return text;
    for (size_t i = 0; i < b->program->nPackages; i++) {
        packageInfo *p = &b->program->packages[i];
        if (p->id != id + 1) continue;
        char hash[65]; buildHashBytes(p->directory, strlen(p->directory), hash);
        return value(b, RAW_POINTER, "%.*s__package_%s%s", (int)(prefix - text), text, hash, end).operand;
    }
    return text;
}
static const char *nativeKindIdentity(Backend *b, Kind kind) {
    const char *key = value(b, RAW_POINTER, "$kind.%u", (unsigned)kind).operand;
    uintptr_t cached;
    if (hashmap_get(b->nativeNames, key, strlen(key), &cached)) return (const char *)cached;
    if (kind < BUFFER) {
        const char *name = value(b, RAW_POINTER, "scalar%u", (unsigned)kind).operand;
        nativeName(b, key, name); return name;
    }
    BufferKind *t = bufferKind(b, kind);
    if (!t) { fail(b, "missing native type identity"); return "invalid"; }
    char *description = NULL; size_t length = 0;
    FILE *out = open_memstream(&description, &length);
    if (!out) { fail(b, "could not describe native type"); return "invalid"; }
    fprintf(out, "shape=%d;fixed=%d;dynamic=%d;string=%d;map=%d;bucket=%d;count=%zu;",
            t->shape, t->fixedArray, t->dynamicArray, t->immutableString, t->map, t->mapEntry, t->count);
    if (t->definition || t->record || t->contract) {
        token name = t->definition ? t->definition->name : t->record ? t->record->name : t->contract->name;
        fprintf(out, "nominal=%s;file=%s;line=%ld;column=%ld;", nativePackageName(b, name.lexeme),
                name.filename ? name.filename : "", name.line, name.column);
        // Named records anchor recursive layouts; their fields are ABI inputs
        // through dependency sources rather than recursively expanded identity.
        for (size_t i = 0; i < t->nArguments; i++) fprintf(out, "argument=%s;", nativeKindIdentity(b, t->arguments[i]));
    } else {
        fprintf(out, "element=%s;requirement=%s;", nativeKindIdentity(b, t->element), nativeKindIdentity(b, t->requirement));
        for (size_t i = 0; i < t->nArguments; i++) fprintf(out, "argument=%s;", nativeKindIdentity(b, t->arguments[i]));
        for (size_t i = 0; i < t->count && t->members; i++) fprintf(out, "member=%s;", nativeKindIdentity(b, t->members[i]));
        if (t->map) fprintf(out, "key=%s;value=%s;keyRequirement=%s;valueRequirement=%s;",
                            nativeKindIdentity(b, t->mapKey), nativeKindIdentity(b, t->mapValue),
                            nativeKindIdentity(b, t->mapKeyRequirement), nativeKindIdentity(b, t->mapValueRequirement));
        if (t->callable) fprintf(out, "variadic=%d;", t->callable->isVariadic);
    }
    fclose(out);
    char hash[65]; buildHashBytes(description, length, hash); free(description);
    nativeName(b, key, hash);
    if (hashmap_get(b->nativeNames, key, strlen(key), &cached)) return (const char *)cached;
    return "invalid";
}
static void nativeConstantIdentity(Backend *b, const char *name, const void *data, size_t size) {
    if (!b->units) return;
    char hash[65]; buildHashBytes(data, size, hash);
    nativeName(b, name, value(b, RAW_POINTER, "@.constant.%s", hash).operand);
}
static const char *nativeFunctionIdentity(Backend *b, struct stmt_func *f, MethodInstance *method) {
    char *description = NULL; size_t length = 0;
    FILE *out = open_memstream(&description, &length);
    if (!out) { fail(b, "could not describe native function"); return "invalid"; }
    fprintf(out, "%s;%s;%ld;%ld;", nativePackageName(b, f->name.lexeme),
            f->name.filename ? f->name.filename : "", f->name.line, f->name.column);
    if (method) {
        fprintf(out, "receiver=%s;", nativeKindIdentity(b, method->receiver));
        for (size_t i = 0; i < method->nBindings; i++)
            fprintf(out, "%s=%s;", method->bindings[i].name, nativeKindIdentity(b, method->bindings[i].kind));
    }
    fclose(out); char hash[65]; buildHashBytes(description, length, hash); free(description);
    return ownOperand(b, strdup(hash));
}
static void nativeDeferredIdentity(Backend *b, unsigned id) {
    if (!b->units) return;
    token at = b->deferCall ? b->deferCall->location : b->location;
    const char *owner = b->function ? nativeFunctionIdentity(b, b->function, b->method) : "globals";
    const char *description = value(b, RAW_POINTER, "%s;%s;%ld;%ld;", owner, at.filename ? at.filename : "", at.line, at.column).operand;
    for (TypeBinding *binding = b->typeBindings; binding; binding = binding->next)
        description = value(b, RAW_POINTER, "%s%s=%s;", description, binding->name, nativeKindIdentity(b, binding->kind)).operand;
    char hash[65]; buildHashBytes(description, strlen(description), hash);
    nativeName(b, value(b, RAW_POINTER, "@__ocean.deferred.%u", id).operand, value(b, RAW_POINTER, "@__ocean.deferred.%s", hash).operand);
    nativeName(b, value(b, RAW_POINTER, "@__ocean.deferred.drop.%u", id).operand, value(b, RAW_POINTER, "@__ocean.deferred.drop.%s", hash).operand);
}
static void nativeSpecializationNames(Backend *b) {
    for (MethodInstance *m = b->methods; m; m = m->next) {
        const char *family = m->function->receiverType.nTokens ? "method" : "function";
        nativeName(b, value(b, RAW_POINTER, "@\"ocean.%s.%u.%s\"", family, m->id, m->function->name.lexeme).operand,
                   value(b, RAW_POINTER, "@\"ocean.%s.%s\"", family, nativeFunctionIdentity(b, m->function, m)).operand);
    }
    for (ClosureInstance *c = b->closures; c; c = c->next) {
        const char *description = value(b, RAW_POINTER, "%s;adapter=%d;context=%s;",
                nativeFunctionIdentity(b, c->function, c->method), c->adapter,
                c->typeContext ? nativeFunctionIdentity(b, c->typeContext->function, c->typeContext) : "none").operand;
        for (size_t i = 0; i < c->nBindings; i++)
            description = value(b, RAW_POINTER, "%s%s=%s;", description, c->bindings[i].name, nativeKindIdentity(b, c->bindings[i].kind)).operand;
        char hash[65]; buildHashBytes(description, strlen(description), hash);
        nativeName(b, value(b, RAW_POINTER, "@__ocean.closure.%u", c->id).operand, value(b, RAW_POINTER, "@__ocean.closure.%s", hash).operand);
    }
    for (TypeConstraint *c = b->constraints; c; c = c->next)
        nativeName(b, value(b, RAW_POINTER, "@__ocean.constraint.%u", c->id).operand,
                   value(b, RAW_POINTER, "@__ocean.constraint.%s.%d", nativeKindIdentity(b, c->pattern), c->inlineBuffer).operand);
}
static const char *nativeSymbol(Backend *b, const char *symbol) {
    uintptr_t mapped;
    if (hashmap_get(b->nativeNames, symbol, strlen(symbol), &mapped)) return (const char *)mapped;
    const char *normalized = !strncmp(symbol, "@\"ocean.", 8) || !strncmp(symbol, "@ocean.", 7)
                                 ? nativePackageName(b, symbol) : symbol;
    if (normalized != symbol) { nativeName(b, symbol, normalized); return normalized; }
    const char *prefixes[] = {"%ocean.struct.", "@.zero.array.", "@__ocean.any.type.", "@__ocean.any.box.",
        "@__ocean.any.unbox.", "@__ocean.any.clone.", "@__ocean.any.destroy.", "@__ocean.closure.cell.drop.", "@__ocean.equal."};
    for (size_t i = 0; i < sizeof(prefixes)/sizeof(*prefixes); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(symbol, prefixes[i], n) || !isdigit((unsigned char)symbol[n])) continue;
        char *end; unsigned long kind = strtoul(symbol + n, &end, 10);
        const char *result = value(b, RAW_POINTER, "%s%s", prefixes[i], nativeKindIdentity(b, (Kind)kind)).operand;
        if (*end == '.' && isdigit((unsigned char)end[1])) {
            unsigned long right = strtoul(end + 1, &end, 10);
            result = value(b, RAW_POINTER, "%s.%s", result, nativeKindIdentity(b, (Kind)right)).operand;
        }
        if (*end) return symbol;
        nativeName(b, symbol, result); return result;
    }
    return symbol;
}
static size_t nativeSymbolLength(const char *p) {
    const char *start = p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p) p++;
    } else while (isalnum((unsigned char)*p) || (*p && strchr("._$-", *p))) p++;
    return (size_t)(p - start);
}
static char *nativeLine(Backend *b, const char *line) {
    char *result = NULL; size_t length = 0;
    FILE *out = open_memstream(&result, &length);
    if (!out) { fail(b, "could not serialize native module"); return strdup(""); }
    while (*line) {
        if (*line == '@' || (*line == '%' && !strncmp(line, "%ocean.struct.", 14))) {
            size_t n = nativeSymbolLength(line);
            char *symbol = strndup(line, n);
            fputs(nativeSymbol(b, symbol), out); free(symbol); line += n;
        } else fputc(*line++, out);
    }
    fclose(out); return result;
}
static char *nativePublicLine(const char *line) {
    const char *where = strstr(line, "private "); size_t length = 8;
    if (!where) { where = strstr(line, "internal "); length = 9; }
    if (!where) return strdup(line);
    char *result = malloc(strlen(line) + 8);
    if (!result) return NULL;
    size_t prefix = (size_t)(where - line);
    memcpy(result, line, prefix); strcpy(result + prefix, "hidden "); strcpy(result + prefix + 7, where + length);
    return result;
}
static void nativeCopy(Backend *b, FILE *from, FILE *to) {
    rewind(from); char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, from) >= 0) {
        char *serialized = nativeLine(b, line); fputs(serialized, to); free(serialized);
    }
    if (ferror(from) || ferror(to)) fail(b, "could not write native package module");
    free(line);
}
static int nativeFreeSet(const void *key, size_t size, uintptr_t val, void *context) {
    (void)size; (void)val; (void)context; free((void *)key); return 0;
}
static void finishNativeModules(Backend *b, FILE *support) {
    nativeSpecializationNames(b);
    // All ordinary signatures have already been lowered. Declare them in the
    // support module, and foreign ordinary functions in each package module.
    FILE *ordinary = tmpfile(), *types = tmpfile(), *declarations = tmpfile(), *runtime = tmpfile();
    if (!ordinary || !types || !declarations || !runtime) {
        if (ordinary) fclose(ordinary);
        if (types) fclose(types);
        if (declarations) fclose(declarations);
        if (runtime) fclose(runtime);
        fail(b, "could not create module prelude"); return;
    }
    b->method = NULL; b->closure = NULL; b->typeBindings = NULL; b->declarationOnly = 1;
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        if (f->isExtern || f->nTypeParams || f->receiverType.nTokens) continue;
        signature(b, f, ordinary); fputc('\n', ordinary);
    }
    b->declarationOnly = 0;
    rewind(support); char *line = NULL; size_t capacity = 0; int skipBody = 0;
    hashmap *definitions = hashmap_create();
    if (!definitions) {
        fclose(ordinary); fclose(types); fclose(declarations); fclose(runtime);
        fail(b, "could not create native definition table"); return;
    }
    while (getline(&line, &capacity, support) >= 0) {
        if (skipBody) { if (!strcmp(line, "}\n")) skipBody = 0; continue; }
        char *serialized = nativeLine(b, line);
        if (!strncmp(serialized, "define ", 7) || serialized[0] == '@') {
            char *symbol = strchr(serialized, '@');
            if (symbol) {
                size_t n = nativeSymbolLength(symbol); uintptr_t present;
                if (hashmap_get(definitions, symbol, n, &present)) {
                    skipBody = !strncmp(serialized, "define ", 7); free(serialized); continue;
                }
                char *saved = strndup(symbol, n); hashmap_set(definitions, saved, n, 1);
            }
        }
        char *public = nativePublicLine(serialized);
        if (!public) { fail(b, "could not expose native symbol"); free(serialized); break; }
        if (serialized[0] != '%') fputs(public, runtime);
        if (serialized[0] == '%') fputs(serialized, types);
        if (!strncmp(serialized, "declare ", 8)) fputs(serialized, declarations);
        else if (!strncmp(serialized, "define ", 7)) {
            char *brace = strrchr(public, '{');
            if (!brace) fail(b, "missing runtime function signature");
            else { *brace = 0; fprintf(declarations, "declare %s\n", public + 7); }
        } else if (serialized[0] == '@' && strncmp(serialized, "@llvm.", 6)) {
            char *end = serialized + nativeSymbolLength(serialized);
            if (strstr(end, "= external ")) fputs(serialized, declarations);
            else fprintf(declarations, "%.*s = external %sglobal i8\n", (int)(end - serialized), serialized,
                         strstr(end, "thread_local ") ? "thread_local " : "");
        }
        free(public); free(serialized);
    }
    free(line); hashmap_iterate(definitions, nativeFreeSet, NULL); hashmap_free(definitions);
    b->units->needsFFI = b->ffiUsed;
    b->units->needsThreads = b->threadsUsed;
    for (size_t i = 0; i < b->program->nStatements; i++) {
        stmt *s = &b->program->statements[i];
        if (s->type != STMT_FUNC) continue;
        struct stmt_func *f = s->value;
        const char *name = f->externalName ? f->externalName : f->name.lexeme;
        if (f->isExtern && !strncmp(name, "curl_", 5)) b->units->needsCurl = 1;
        if (f->isExtern && !strncmp(name, "ocean_process_", 14)) b->units->needsProcess = 1;
    }
    for (size_t unit = 0; unit < b->units->count; unit++) {
        if (!b->units->needed[unit]) continue;
        FILE *out = fopen(b->units->paths[unit], "w");
        if (!out) { fail(b, "could not create native module"); break; }
        emitTarget(out);
        if (!unit && b->units->needsProcess) fputs("define hidden void @ocean_runtime_process_marker() { ret void }\n", out);
        if (!unit) {
            copy(types, out); nativeCopy(b, ordinary, out); copy(runtime, out);
        } else {
            copy(types, out); copy(declarations, out);
            b->declarationOnly = 1;
            for (size_t i = 0; i < b->program->nStatements; i++) {
                stmt *s = &b->program->statements[i];
                if (s->type != STMT_FUNC) continue;
                struct stmt_func *f = s->value;
                if (f->isExtern || f->nTypeParams || f->receiverType.nTokens) continue;
                size_t owner = 0;
                for (size_t j = 0; j < b->program->nSources; j++)
                    if (f->name.filename && !strcmp(f->name.filename, b->program->sources[j].filename)) owner = b->program->sources[j].packageId;
                if (owner == b->program->packages[unit - 1].id) continue;
                FILE *declaration = tmpfile();
                if (!declaration) { fail(b, "could not declare imported function"); break; }
                signature(b, f, declaration); fputc('\n', declaration);
                nativeCopy(b, declaration, out); fclose(declaration);
            }
            b->declarationOnly = 0;
            nativeCopy(b, b->packageBodies[unit], out);
        }
        if (fclose(out)) fail(b, "could not finish native module");
    }
    fclose(ordinary); fclose(types); fclose(declarations); fclose(runtime);
}
