// Function values have one stable ABI: { code pointer, environment pointer }.
// Concrete parameter/result kinds remain part of the compiler's type identity.
static Kind closureKind(Backend *b, struct stmt_func *function) {
    for (size_t i = 0; i < function->nTypeParams; i++) {
        TypeBinding *binding = b->typeBindings;
        while (binding && strcmp(binding->name, function->typeParamNames[i].lexeme)) binding = binding->next;
        if (!binding) {
            fail(b, "generic functions require explicit type arguments before use as values");
            return BAD;
        }
    }
    Kind result = resultType(b, function);
    Kind *parameters = checked(sizeof(Kind) * (function->nParams + 1));
    for (size_t i = 0; i < function->nParams; i++) parameters[i] = parameterKind(b, function, i);
    for (BufferKind *kind = b->buffers; kind; kind = kind->next) {
        if (kind->shape != 17 || kind->element != result || kind->nArguments != function->nParams ||
            kind->callable->isVariadic != function->isVariadic) continue;
        size_t i = 0;
        while (i < function->nParams && kind->arguments[i] == parameters[i]) i++;
        if (i == function->nParams) { free(parameters); return kind->kind; }
    }
    BufferKind *kind = checked(sizeof(*kind));
    kind->kind = (Kind)(BUFFER + b->nBuffers++); kind->shape = 17;
    kind->element = result; kind->arguments = parameters; kind->nArguments = function->nParams;
    kind->ir = strdup("{ ptr, ptr }"); kind->count = 2;
    kind->members = checked(2 * sizeof(Kind)); kind->members[0] = kind->members[1] = RAW_POINTER;
    kind->callable = checked(sizeof(*kind->callable)); *kind->callable = *function;
    kind->callable->isExtern = 0; kind->callable->nTypeParams = 0; kind->callable->receiverType = (typeTokens){0};
    kind->callable->name.lexeme = "<function>"; kind->callable->displayName = "<function>";
    kind->next = b->buffers; b->buffers = kind;
    return kind->kind;
}

static Value specializedClosureValue(Backend *b, expr *expression, int kindOnly) {
    typeTokens *arguments; size_t count;
    struct stmt_func *function = directCallee(b, expression, &arguments, &count);
    if (!function) { fail(b, "expected a concrete function specialization"); return value(b, BAD, "0"); }
    MethodInstance *instance = functionInstance(b, function, arguments, count);
    if (!instance) return value(b, BAD, "0");
    TypeBinding *previous = b->typeBindings; b->typeBindings = instance->bindings;
    Value result = kindOnly ? value(b, closureKind(b, function), "") : closureValue(b, function, instance, 1);
    b->typeBindings = previous;
    return result;
}

static Value closureParts(Backend *b, Value closure, int environment) {
    Value result = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = extractvalue { ptr, ptr } %s, %d\n", result.operand, closure.operand, environment);
    return result;
}

static void requireClosureCode(Backend *b, Value code, token location) {
    Value present = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ne ptr %s, null\n", present.operand, code.operand);
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, present, good, bad); label(b, bad);
    const char *message = "call of an uninitialized function";
    Value text = moduleString(b, message);
    location = diagnosticLocation(b, location);
    Value file = moduleString(b, location.filename);
    char *source = sourceSnippet(b, location);
    Value snippet = moduleString(b, source); free(source);
    b->boundsUsed = 1;
    fprintf(b->body, "  call void @__ocean.panic(ptr %s, i64 %zu, ptr %s, i64 %ld, i64 %ld, ptr %s)\n  unreachable\n",
            text.operand, strlen(message), file.operand, location.line, location.column, snippet.operand);
    b->terminated = 1; label(b, good);
}

static Value localSlot(Backend *b, Kind kind, const char *name) {
    int captured = 0;
    for (size_t i = 0; b->function && name && i < b->function->nCapturedLocals; i++)
        if (!strcmp(b->function->capturedLocals[i].lexeme, name)) captured = 1;
    if (!captured) return slot(b, kind);
    b->closuresUsed = 1;
    ClosureCellType *registered = b->closureCells;
    while (registered && registered->kind != kind) registered = registered->next;
    if (!registered) {
        registered = checked(sizeof(*registered));
        *registered = (ClosureCellType){kind, b->closureCells}; b->closureCells = registered;
    }
    Value result = temp(b, kind), size = storageSize(b, kind);
    fprintf(b->body, "  %s = call ptr @__ocean.closure_alloc(i64 %s, ptr @__ocean.closure.cell.drop.%u)\n",
            result.operand, size.operand, (unsigned)kind);
    result.owner = b->globalScope; result.throughPointer = result.closureCell = 1;
    rawStore(b, result, emptyValue(b, kind));
    return result;
}

static Value closureValue(Backend *b, struct stmt_func *function, MethodInstance *method, int adapter) {
    Kind kind = closureKind(b, function);
    if (kind == BAD) return value(b, BAD, "0");
    if (adapter)
        for (ClosureInstance *existing = b->closures; existing; existing = existing->next)
            if (existing->adapter && existing->function == function && existing->method == method && existing->kind == kind)
                return value(b, kind, "{ ptr @__ocean.closure.%u, ptr null }", existing->id);
    ClosureInstance *instance = checked(sizeof(*instance));
    instance->function = function; instance->kind = kind; instance->id = ++b->nClosures;
    instance->adapter = adapter; instance->method = method;
    instance->typeContext = b->closure ? b->closure->typeContext : b->method;
    Scope **tail = &instance->lexicalScope;
    for (Scope *scope = b->scope; scope && scope != b->globalScope; scope = scope->parent) {
        Scope *snapshot = checked(sizeof(*snapshot)); snapshot->declarations = scope->declarations;
        *tail = snapshot; tail = &snapshot->parent;
    }
    *tail = b->globalScope;
    for (TypeBinding *binding = b->typeBindings; binding; binding = binding->next) instance->nBindings++;
    instance->bindings = checked(sizeof(TypeBinding) * (instance->nBindings + 1));
    size_t index = 0;
    for (TypeBinding *binding = b->typeBindings; binding; binding = binding->next, index++) {
        instance->bindings[index] = *binding;
        instance->bindings[index].next = index + 1 < instance->nBindings ? &instance->bindings[index+1] : NULL;
    }
    Value environment = value(b, RAW_POINTER, "null");
    if (!adapter && function->nCaptures) {
        b->closuresUsed = 1;
        instance->captures = checked(sizeof(Kind) * function->nCaptures);
        instance->captureLengths = checked(function->nCaptures);
        environment = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = call ptr @__ocean.closure_alloc(i64 %zu, ptr null)\n", environment.operand,
                function->nCaptures * 16);
        for (size_t i = 0; i < function->nCaptures; i++) {
            Value address = find(b, function->captures[i].lexeme);
            if (!address.closureCell) fail(b, "captured variable has no shared storage");
            instance->captures[i] = address.kind;
            Value field = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %s, i64 %zu, i32 0\n", field.operand, environment.operand, i);
            fprintf(b->body, "  store ptr %s, ptr %s\n", address.operand, field.operand);
            instance->captureLengths[i] = address.length != NULL;
            Value lengthField = temp(b, RAW_POINTER);
            fprintf(b->body, "  %s = getelementptr { ptr, i64 }, ptr %s, i64 %zu, i32 1\n", lengthField.operand, environment.operand, i);
            fprintf(b->body, "  store i64 %s, ptr %s\n", address.length ? address.length : "0", lengthField.operand);
        }
    }
    if (b->lastClosure) b->lastClosure->next = instance; else b->closures = instance;
    b->lastClosure = instance;
    Value result = temp(b, kind), complete = temp(b, kind);
    fprintf(b->body, "  %s = insertvalue { ptr, ptr } poison, ptr @__ocean.closure.%u, 0\n", result.operand, instance->id);
    fprintf(b->body, "  %s = insertvalue { ptr, ptr } %s, ptr %s, 1\n", complete.operand, result.operand, environment.operand);
    return complete;
}

static BufferKind *closureCalleeKind(Backend *b, expr *callee) {
    if (callee->type == EXPR_TYPE_IDENTIFIER && !lookupBinding(b, ((struct expr_identifier *)callee->underlying)->lexeme.lexeme)) return NULL;
    if (callee->type == EXPR_TYPE_INSTANTIATION) return NULL;
    if (callee->type == EXPR_TYPE_DOT) {
        struct expr_dot *dot = callee->underlying;
        BufferKind *parent = bufferKind(b, expressionKind(b, &dot->parent));
        while (parent && parent->shape == 6) parent = bufferKind(b, parent->element);
        if (!parent || parent->shape != 5 || recordField(parent, ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme) == parent->count) return NULL;
    }
    BufferKind *kind = bufferKind(b, expressionKind(b, callee));
    return kind && kind->shape == 17 ? kind : NULL;
}

static void emitClosureAdapter(Backend *b, ClosureInstance *instance, FILE *out) {
    struct stmt_func *function = instance->function;
    BufferKind *kind = bufferKind(b, instance->kind);
    fprintf(out, "define internal %s @__ocean.closure.%u(ptr %%environment", irType(b, kind->element), instance->id);
    for (size_t i = 0; i < kind->nArguments; i++) fprintf(out, ", %s %%arg%zu", irType(b, kind->arguments[i]), i);
    fputs(") {\nentry:\n  ", out);
    if (kind->element != VOID) fputs("%result = ", out);
    fprintf(out, "call %s ", irType(b, kind->element));
    if (instance->method) methodName(out, instance->method); else functionName(b, out, function);
    fputc('(', out);
    for (size_t i = 0; i < kind->nArguments; i++) fprintf(out, "%s%s %%arg%zu", i ? ", " : "", irType(b, kind->arguments[i]), i);
    fputs(")\n", out);
    if (kind->element == VOID) fputs("  ret void\n}\n", out);
    else fprintf(out, "  ret %s %%result\n}\n", irType(b, kind->element));
}

static void emitClosureCellDrops(Backend *b, FILE *output) {
    for (ClosureCellType *cell = b->closureCells; cell; cell = cell->next) {
        Scope scope = {.parent = b->globalScope}; b->scope = &scope;
        b->body = tmpfile(); b->slots = tmpfile(); b->boxRegion = (Value){0}; b->terminated = 0;
        DynamicArrayTemporary *temps = b->dynamicArrayTemps; b->dynamicArrayTemps = NULL;
        int previous = b->copyTransport; b->copyTransport = 3;
        copyValue(b, load(b, value(b, cell->kind, "%%cell")), 0);
        b->copyTransport = previous;
        fputs("  ret void\n", b->body);
        fprintf(output, "define internal void @__ocean.closure.cell.drop.%u(ptr %%cell) {\nentry:\n", (unsigned)cell->kind);
        copy(b->slots, output); copy(b->body, output); fputs("}\n", output);
        fclose(b->slots); fclose(b->body); clearScope(&scope); b->scope = NULL;
        b->dynamicArrayTemps = temps;
    }
}

// Captures use a process-lifetime region, including cycles and escaped closures.
// Shutdown releases owned cell contents before freeing their region storage.
static void emitClosureRuntime(Backend *b) {
    if (!b->closuresUsed) return;
    fputs(
        "@__ocean.closure_head = internal global ptr null\n"
        "@__ocean.closure_registered = internal global i8 0\n", b->out);
    fprintf(b->out, "declare i32 %s(ptr)\n", libcSymbol(b, "atexit"));
    fputs(
        "define internal ptr @__ocean.closure_alloc(i64 %size, ptr %drop) {\n"
        "entry:\n"
        "  %registration = cmpxchg ptr @__ocean.closure_registered, i8 0, i8 1 acq_rel acquire\n"
        "  %first = extractvalue { i8, i1 } %registration, 1\n"
        "  br i1 %first, label %register, label %wait\n"
        "register:\n", b->out);
    fprintf(b->out, "  call i32 %s(ptr @__ocean.closure_cleanup)\n", libcSymbol(b, "atexit"));
    fputs(
        "  store atomic i8 2, ptr @__ocean.closure_registered release, align 1\n"
        "  br label %allocate\n"
        "wait:\n"
        "  %state = load atomic i8, ptr @__ocean.closure_registered acquire, align 1\n"
        "  %ready = icmp eq i8 %state, 2\n"
        "  br i1 %ready, label %allocate, label %wait\n"
        "allocate:\n"
        "  %total = add i64 %size, 16\n", b->out);
    fprintf(b->out, "  %%node = call ptr %s(i64 %%total)\n", libcSymbol(b, "malloc"));
    fputs(
        "  %drop.slot = getelementptr { ptr, ptr }, ptr %node, i32 0, i32 1\n"
        "  store ptr %drop, ptr %drop.slot\n"
        "  br label %publish\n"
        "publish:\n"
        "  %head = load atomic ptr, ptr @__ocean.closure_head acquire, align 8\n"
        "  store ptr %head, ptr %node\n"
        "  %exchange = cmpxchg ptr @__ocean.closure_head, ptr %head, ptr %node acq_rel acquire\n"
        "  %published = extractvalue { ptr, i1 } %exchange, 1\n"
        "  br i1 %published, label %done, label %publish\n"
        "done:\n"
        "  %payload = getelementptr i8, ptr %node, i64 16\n"
        "  ret ptr %payload\n"
        "}\n"
        "define internal void @__ocean.closure_cleanup() {\nentry:\n"
        "  br label %loop\nloop:\n"
        "  %node = load ptr, ptr @__ocean.closure_head\n"
        "  %present = icmp ne ptr %node, null\n"
        "  br i1 %present, label %inspect, label %done\ninspect:\n"
        "  %next = load ptr, ptr %node\n"
        "  store ptr %next, ptr @__ocean.closure_head\n"
        "  %drop.slot = getelementptr { ptr, ptr }, ptr %node, i32 0, i32 1\n"
        "  %drop = load ptr, ptr %drop.slot\n"
        "  %has.drop = icmp ne ptr %drop, null\n"
        "  br i1 %has.drop, label %destroy, label %free\ndestroy:\n"
        "  %payload = getelementptr i8, ptr %node, i64 16\n"
        "  call void %drop(ptr %payload)\n"
        "  br label %free\nfree:\n", b->out);
    fprintf(b->out, "  call void %s(ptr %%node)\n", libcSymbol(b, "free"));
    fputs(
        "  br label %loop\ndone:\n  ret void\n}\n", b->out);
}
