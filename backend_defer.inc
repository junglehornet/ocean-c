// Named results can be changed by deferred callees between setjmp and longjmp.
// Volatile reads prevent SSA promotion from reusing their initial values.
static Value loadRecoveredResult(Backend *b, Value address) {
    Value result = temp(b, address.kind);
    fprintf(b->body, "  %s = load volatile %s, ptr %s\n", result.operand, irType(b, result.kind), address.operand);
    return result;
}

static int containsDefer(stmt *s) {
    if (!s) return 0;
    switch (s->type) {
    case STMT_DEFER: return 1;
    case STMT_LABEL: return containsDefer(((struct stmt_label *)s->value)->body);
    case STMT_BLOCK: {
        struct stmt_block *block = s->value;
        for (size_t i = 0; i < block->nStmts; i++) if (containsDefer(&block->stmts[i])) return 1;
        return 0;
    }
    case STMT_IF: {
        struct stmt_if *node = s->value;
        return containsDefer(node->preStmt) || containsDefer(node->ifTrue) || containsDefer(node->ifFalse);
    }
    case STMT_FOR: {
        struct stmt_for *node = s->value;
        return containsDefer(node->initializer) || containsDefer(node->body);
    }
    case STMT_SWITCH: {
        struct stmt_switch *node = s->value;
        if (containsDefer(&node->initializer)) return 1;
        for (size_t i = 0; i < node->nCases; i++) {
            stmt body = {.type = STMT_BLOCK, .value = &node->cases[i].body};
            if (containsDefer(&body)) return 1;
        }
        return 0;
    }
    default: return 0;
    }
}

// A pointer captured by defer may outlive its lexical block. Move cleanup of
// an addressed local to the function scope and retain its backing stack storage.
static void retainDeferredPointee(Backend *b, Value pointer) {
    BufferKind *kind = bufferKind(b, pointer.kind);
    if (!kind || kind->shape != 6) return;
    Scope *functionScope = b->functionScope;
    if (!functionScope) return;
    for (Scope *scope = b->scope; scope && scope != functionScope; scope = scope->parent)
        for (Binding *binding = scope->bindings; binding; binding = binding->next)
            if (binding->autoRelease && !strcmp(binding->address.operand, pointer.operand)) {
                Binding *retained = checked(sizeof(*retained));
                *retained = *binding;
                retained->name = "$deferred_pointee";
                retained->next = functionScope->bindings;
                functionScope->bindings = retained;
                binding->autoRelease = 0;
                for (Scope *keep = b->scope; keep && keep != b->globalScope; keep = keep->parent)
                    keep->retainAllocations = 1;
                return;
            }
}

// Deferred calls own a heap capture so loop and block stack restoration cannot
// invalidate evaluated arguments. Each execution pushes a separate capture.
static void scheduleDefer(Backend *b, struct stmt_func *f, MethodInstance *method,
                          Value receiver, Value *args, size_t nArgs, Kind result,
                          size_t fixed, Kind *parameterKinds, expr *intrinsic, Value callable) {
    unsigned id = ++b->nDefers;
    int consuming = intrinsic && ((struct expr_call *)intrinsic->underlying)->func.type == EXPR_TYPE_IDENTIFIER &&
        !strcmp(((struct expr_identifier *)((struct expr_call *)intrinsic->underlying)->func.underlying)->lexeme.lexeme, "free");
    int indirect = callable.kind != BAD;
    size_t offset = indirect || (method && f->receiverType.nTokens) ? 1 : 0, count = nArgs + offset;
    size_t callCount = count;
    Value *captures = checked(sizeof(Value) * (2 * count + 1));
    if (offset) captures[0] = indirect ? callable : receiver;
    for (size_t i = 0; i < nArgs; i++) captures[i + offset] = args[i];
    for (size_t i = 0; i < callCount; i++) retainDeferredPointee(b, captures[i]);
    size_t *backing = checked(sizeof(size_t) * (callCount + 1));
    for (size_t i = 0; i < callCount; i++) {
        backing[i] = 0;
        if (captures[i].backingKind) {
            backing[i] = count;
            captures[count++] = value(b, captures[i].backingKind, "%s", captures[i].backingOperand);
        }
    }
    char *layout = NULL;
    size_t length = 0;
    FILE *description = open_memstream(&layout, &length);
    fputs("{ ptr, ptr, ptr", description);
    for (size_t i = 0; i < count; i++) fprintf(description, ", %s", irType(b, captures[i].kind));
    fputs(" }", description); fclose(description);
    Value capture = temp(b, RAW_POINTER);
    fprintf(b->body, "  %s = call ptr @\"\\01malloc\"(i64 ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64))\n", capture.operand, layout);
    Value *copies = checked(sizeof(Value) * (count + 1));
    for (size_t remaining = count; remaining > 0; remaining--) {
        size_t i = remaining - 1;
        int previous = b->copyTransport; b->copyTransport = 1;
        Value copied;
        if (i < callCount && backing[i]) {
            copied = arrayData(b, copies[backing[i]]); copied.kind = captures[i].kind;
        } else copied = consuming ? captures[i] : copyValue(b, captures[i], 0);
        b->copyTransport = previous;
        escapeDynamicArrays(b, copied);
        copies[i] = copied;
        Value field = temp(b, RAW_POINTER);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 %zu\n", field.operand, layout, capture.operand, i + 3);
        rawStore(b, value(b, copied.kind, "%s", field.operand), copied);
    }
    free(copies); free(backing);
    fprintf(b->body, "  call void @__ocean.defer_push(ptr %s, ptr %s, ptr @__ocean.deferred.%u, ptr @__ocean.deferred.drop.%u)\n", b->deferFrame.operand, capture.operand, id, id);
    FILE *body = b->body, *slots = b->slots;
    Scope *scope = b->scope, thunkScope = {.parent = b->globalScope};
    DynamicArrayTemporary *temps = b->dynamicArrayTemps;
    Value region = b->boxRegion;
    int terminated = b->terminated;
    b->body = tmpfile(); b->slots = tmpfile(); b->scope = &thunkScope;
    b->dynamicArrayTemps = NULL; b->boxRegion = (Value){0}; b->terminated = 0;
    for (size_t i = 0; i < count; i++) {
        Value field = temp(b, captures[i].kind);
        fprintf(b->body, "  %s = getelementptr %s, ptr %%capture, i32 0, i32 %zu\n", field.operand, layout, i + 3);
        captures[i] = load(b, field);
    }
    Value returned = value(b, VOID, "");
    if (intrinsic) {
        struct expr_call original = *(struct expr_call *)intrinsic->underlying;
        struct expr_call call = original;
        size_t receiverCount = original.func.type == EXPR_TYPE_DOT ? 1 : 0;
        call.nParams = callCount - receiverCount;
        call.params = checked(sizeof(expr) * (call.nParams + 1));
        struct expr_identifier *identifiers = checked(sizeof(*identifiers) * (callCount + 1));
        for (size_t i = 0; i < callCount; i++) {
            identifiers[i].lexeme = (token){.type = IDENTIFIER, .lexeme = (char *)value(b, RAW_POINTER, "$deferred%zu", i).operand};
            Value address = slot(b, captures[i].kind);
            rawStore(b, address, captures[i]);
            bind(b, identifiers[i].lexeme.lexeme, address);
            if (i >= receiverCount)
                call.params[i - receiverCount] = (expr){.type = EXPR_TYPE_IDENTIFIER, .underlying = &identifiers[i]};
        }
        struct expr_dot dot;
        struct expr_unary dereference = {0};
        if (receiverCount) {
            dot = *(struct expr_dot *)original.func.underlying;
            expr receiverValue = {.type = EXPR_TYPE_IDENTIFIER, .underlying = &identifiers[0]};
            if (strcmp(((struct expr_identifier *)dot.field.underlying)->lexeme.lexeme, "array")) {
                dereference = (struct expr_unary){.operation = UNARY_DEREFERENCE, .child = receiverValue};
                receiverValue = (expr){.type = EXPR_TYPE_UNARY, .underlying = &dereference};
            }
            dot.parent = receiverValue;
            call.func.underlying = &dot;
        }
        expr invoke = *intrinsic; invoke.underlying = &call;
        returned = expression(b, &invoke);
        free(identifiers); free(call.params);
    } else {
        Value code = value(b, RAW_POINTER, "null"), environment = code;
        if (indirect) { code = closureParts(b, captures[0], 0); environment = closureParts(b, captures[0], 1); requireClosureCode(b, code, b->location); }
        Value parent = value(b, RAW_POINTER, "null");
        if (!f->isExtern) parent = pushTraceFrame(b, f, b->location);
        returned = result == VOID ? value(b, VOID, "") : temp(b, result);
        fputs("  ", b->body);
        if (result != VOID) fprintf(b->body, "%s = ", returned.operand);
        fprintf(b->body, "call %s ", irType(b, result));
        if (f->isExtern && f->isVariadic) {
            fputc('(', b->body);
            for (size_t i = 0; i < fixed; i++) fprintf(b->body, "%s%s", i ? ", " : "", irType(b, parameterKinds[i]));
            fprintf(b->body, "%s...) ", fixed ? ", " : "");
        }
        if (indirect) fprintf(b->body, "%s", code.operand);
        else if (method) methodName(b->body, method); else functionName(b, b->body, f);
        fputc('(', b->body);
        if (indirect) fprintf(b->body, "ptr %s", environment.operand);
        for (size_t i = indirect ? 1 : 0; i < callCount; i++) fprintf(b->body, "%s%s %s", (i > (indirect ? 1 : 0) || indirect) ? ", " : "", irType(b, captures[i].kind), captures[i].operand);
        fputs(")\n", b->body);
        if (!f->isExtern) popTraceFrame(b, parent);
    }
    int previous = b->copyTransport; b->copyTransport = 3;
    if ((!f || !f->isExtern) && returned.kind != VOID) copyValue(b, returned, 0);
    b->copyTransport = previous;
    cleanDynamicArrayTemps(b, NULL);
    if (b->boxRegion.operand)
        fprintf(b->body, "  call void @__ocean.any.clean(ptr %s, ptr null)\n", b->boxRegion.operand);
    fputs("  ret void\n", b->body);
    fprintf(b->deferredOutput, "define internal void @__ocean.deferred.%u(ptr %%capture) {\nentry:\n", id);
    copy(b->slots, b->deferredOutput); copy(b->body, b->deferredOutput); fputs("}\n", b->deferredOutput);
    fclose(b->slots); fclose(b->body); clearScope(&thunkScope);
    b->body = tmpfile(); b->slots = tmpfile(); b->boxRegion = (Value){0};
    b->terminated = 0;
    // Separate destruction lets forwarding discard calls without invoking them,
    // and lets a nested panic release a call whose thunk it abandons.
    if (!consuming) for (size_t i = 0; i < count; i++) {
        Value field = temp(b, captures[i].kind);
        fprintf(b->body, "  %s = getelementptr %s, ptr %%capture, i32 0, i32 %zu\n", field.operand, layout, i + 3);
        int previous = b->copyTransport; b->copyTransport = 3;
        copyValue(b, load(b, field), 0); b->copyTransport = previous;
    }
    fputs("  ret void\n", b->body);
    fprintf(b->deferredOutput, "define internal void @__ocean.deferred.drop.%u(ptr %%capture) {\nentry:\n", id);
    copy(b->slots, b->deferredOutput); copy(b->body, b->deferredOutput); fputs("}\n", b->deferredOutput);
    fclose(b->slots); fclose(b->body);

    while (b->dynamicArrayTemps) {
        DynamicArrayTemporary *next = b->dynamicArrayTemps->next;
        free(b->dynamicArrayTemps); b->dynamicArrayTemps = next;
    }
    b->body = body; b->slots = slots; b->scope = scope; b->dynamicArrayTemps = temps;
    b->boxRegion = region; b->terminated = terminated;
    free(captures); free(layout);
}

static Value scheduleIntrinsicDefer(Backend *b, expr *callExpression) {
    struct expr_call *call = callExpression->underlying;
    // Deferring recover itself cannot recover a panic: it must be called by
    // the body of a deferred Ocean function.
    if (call->func.type == EXPR_TYPE_IDENTIFIER &&
        !strcmp(((struct expr_identifier *)call->func.underlying)->lexeme.lexeme, "recover"))
        return value(b, VOID, "");
    size_t receiver = call->func.type == EXPR_TYPE_DOT ? 1 : 0;
    Value *args = checked(sizeof(Value) * (call->nParams + receiver + 1));
    if (receiver) {
        struct expr_dot *dot = call->func.underlying;
        const char *name = ((struct expr_identifier *)dot->field.underlying)->lexeme.lexeme;
        if (!strcmp(name, "array")) args[0] = expression(b, &dot->parent);
        else {
            Value storage = address(b, &dot->parent);
            args[0] = value(b, pointerKind(b, storage.kind), "%s", storage.operand);
        }
    }
    for (size_t i = 0; i < call->nParams; i++) args[i + receiver] = expression(b, &call->params[i]);
    if (!receiver && !strcmp(((struct expr_identifier *)call->func.underlying)->lexeme.lexeme, "free") && call->nParams == 1) {
        BufferKind *kind = bufferKind(b, args[0].kind);
        if (kind && kind->dynamicArray && addressable(&call->params[0])) {
            escapeDynamicArrays(b, args[0]);
            rawStore(b, address(b, &call->params[0]), emptyValue(b, args[0].kind));
        }
    }
    scheduleDefer(b, NULL, NULL, value(b, BAD, ""), args, call->nParams + receiver, VOID, 0, NULL, callExpression, value(b, BAD, ""));
    free(args);
    return value(b, VOID, "");
}

// The native driver and recovery buffers use the compiler host's libc ABI.
static void emitDeferRuntime(Backend *b) {
    fputs(
        "@__ocean.defer_head = internal thread_local global ptr null\n"
        "@__ocean.recovery_head = internal thread_local global ptr null\n"
        "declare i32 @\"\\01_setjmp\"(ptr) returns_twice\n"
        "declare void @\"\\01longjmp\"(ptr, i32) noreturn\n"
        "\n"
        "define internal void @__ocean.enter(ptr %frame) {\n"
        "  %parent = load ptr, ptr @__ocean.defer_head\n"
        "  store ptr %parent, ptr %frame\n"
        "  %head = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 1\n"
        "  store ptr null, ptr %head\n"
        "  %trace = load ptr, ptr @__ocean.trace_head\n"
        "  %trace.slot = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 2\n"
        "  store ptr %trace, ptr %trace.slot\n"
        "  %context = load ptr, ptr @__ocean.recovery_head\n"
        "  %context.slot = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 4\n"
        "  store ptr %context, ptr %context.slot\n"
        "  store ptr %frame, ptr @__ocean.defer_head\n"
        "  ret void\n"
        "}\n"
        "define internal void @__ocean.defer_push(ptr %frame, ptr %capture, ptr %thunk, ptr %drop) {\n"
        "  %head = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 1\n"
        "  %previous = load ptr, ptr %head\n"
        "  store ptr %previous, ptr %capture\n"
        "  %call = getelementptr { ptr, ptr }, ptr %capture, i32 0, i32 1\n"
        "  store ptr %thunk, ptr %call\n"
        "  %drop.slot = getelementptr { ptr, ptr, ptr }, ptr %capture, i32 0, i32 2\n"
        "  store ptr %drop, ptr %drop.slot\n"
        "  store ptr %capture, ptr %head\n"
        "  ret void\n"
        "}\n"
        "define internal i1 @__ocean.run_defers(ptr %frame, ptr %message, i64 %size, i1 %panic) {\n"
        "entry:\n"
        "  %context = alloca %__ocean.recovery\n"
        "  %previous = load ptr, ptr @__ocean.recovery_head\n"
        "  store ptr %previous, ptr %context\n"
        "  %owner = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 1\n"
        "  store ptr %frame, ptr %owner\n"
        "  %active = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 2\n"
        "  store i1 %panic, ptr %active\n"
        "  %string = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 3\n"
        "  store i64 %size, ptr %string\n"
        "  %capacity = getelementptr { i64, i64, ptr }, ptr %string, i32 0, i32 1\n"
        "  store i64 %size, ptr %capacity\n"
        "  %data = getelementptr { i64, i64, ptr }, ptr %string, i32 0, i32 2\n"
        "  store ptr %message, ptr %data\n"
        "  %pending = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 4\n"
        "  store ptr null, ptr %pending\n"
        "  store ptr %context, ptr @__ocean.recovery_head\n"
        "  %head = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 1\n"
        "  br label %loop\n"
        "loop:\n"
        "  %capture = load ptr, ptr %head\n"
        "  %present = icmp ne ptr %capture, null\n"
        "  br i1 %present, label %invoke, label %done\n"
        "invoke:\n"
        "  %next = load ptr, ptr %capture\n"
        "  store ptr %next, ptr %head\n"
        "  %call.slot = getelementptr { ptr, ptr }, ptr %capture, i32 0, i32 1\n"
        "  %call = load ptr, ptr %call.slot\n"
        "  store ptr %capture, ptr %pending\n"
        "  call void %call(ptr %capture)\n"
        "  store ptr null, ptr %pending\n"
        "  call void @__ocean.dispose_capture(ptr %capture)\n"
        "  br label %loop\n"
        "done:\n"
        "  %remaining = load i1, ptr %active\n"
        "  %recovered = xor i1 %remaining, true\n"
        "  %stopped = and i1 %panic, %recovered\n"
        "  store ptr %previous, ptr @__ocean.recovery_head\n"
        "  ret i1 %stopped\n"
        "}\n"
        "define internal ptr @__ocean.recover() {\n"
        "entry:\n"
        "  %context = load ptr, ptr @__ocean.recovery_head\n"
        "  %present = icmp ne ptr %context, null\n"
        "  br i1 %present, label %check, label %none\n"
        "check:\n"
        "  %frame = load ptr, ptr @__ocean.defer_head\n"
        "  %parent = load ptr, ptr %frame\n"
        "  %owner.slot = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 1\n"
        "  %owner = load ptr, ptr %owner.slot\n"
        "  %direct = icmp eq ptr %parent, %owner\n"
        "  %active.slot = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 2\n"
        "  %active = load i1, ptr %active.slot\n"
        "  %allowed = and i1 %direct, %active\n"
        "  br i1 %allowed, label %take, label %none\n"
        "take:\n"
        "  store i1 false, ptr %active.slot\n"
        "  %string = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 3\n"
        "  ret ptr %string\n"
        "none:\n"
        "  ret ptr null\n"
        "}\n"
        "define internal void @__ocean.leave(ptr %frame) {\n"
        "  %ignored = call i1 @__ocean.run_defers(ptr %frame, ptr null, i64 0, i1 false)\n"
        "  %parent = load ptr, ptr %frame\n"
        "  store ptr %parent, ptr @__ocean.defer_head\n"
        "  ret void\n"
        "}\n"
        "define internal void @__ocean.finish_recovery(ptr %frame) {\n"
        "  %parent = load ptr, ptr %frame\n"
        "  store ptr %parent, ptr @__ocean.defer_head\n"
        "  %trace.slot = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 2\n"
        "  %trace = load ptr, ptr %trace.slot\n"
        "  store ptr %trace, ptr @__ocean.trace_head\n"
        "  %context.slot = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 4\n"
        "  %context = load ptr, ptr %context.slot\n"
        "  store ptr %context, ptr @__ocean.recovery_head\n"
        "  ret void\n"
        "}\n"
        "define internal void @__ocean.unwind(ptr %message, i64 %size, i1 %returned) {\n"
        "entry:\n"
        "  %initial = load ptr, ptr @__ocean.defer_head\n"
        "  br label %loop\n"
        "loop:\n"
        "  %frame = phi ptr [ %initial, %entry ], [ %parent, %next ]\n"
        "  %skip = phi i1 [ %returned, %entry ], [ %forward, %next ]\n"
        "  %present = icmp ne ptr %frame, null\n"
        "  br i1 %present, label %select, label %done\n"
        "select:\n"
        "  store ptr %frame, ptr @__ocean.defer_head\n"
        "  call void @__ocean.abandon_capture(ptr %frame)\n"
        "  br i1 %skip, label %next, label %run\n"
        "run:\n"
        "  %recovered = call i1 @__ocean.run_defers(ptr %frame, ptr %message, i64 %size, i1 true)\n"
        "  br i1 %recovered, label %resume, label %next\n"
        "resume:\n"
        "  %environment = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 3\n"
        "  %buffer = load ptr, ptr %environment\n"
        "  call void @\"\\01longjmp\"(ptr %buffer, i32 1)\n"
        "  unreachable\n"
        "next:\n"
        "  call void @__ocean.discard_defers(ptr %frame)\n"
        "  %parent = load ptr, ptr %frame\n"
        "  %trace.slot = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 2\n"
        "  %trace = load ptr, ptr %trace.slot\n"
        "  %forward.slot = getelementptr %__ocean.trace, ptr %trace, i32 0, i32 6\n"
        "  %forward.flag = load i1, ptr %forward.slot\n"
        "  %forward = and i1 %skip, %forward.flag\n"
        "  br label %loop\n"
        "done:\n"
        "  ret void\n"
        "}\n"
        "\n"
        "; Dropping captures never invokes user code.\n"
        "define internal void @__ocean.dispose_capture(ptr %capture) {\n"
        "  %drop.slot = getelementptr { ptr, ptr, ptr }, ptr %capture, i32 0, i32 2\n"
        "  %drop = load ptr, ptr %drop.slot\n"
        "  call void %drop(ptr %capture)\n"
        "  call void @\"\\01free\"(ptr %capture)\n"
        "  ret void\n"
        "}\n"
        "define internal void @__ocean.discard_defers(ptr %frame) {\n"
        "entry:\n"
        "  %head = getelementptr %__ocean.defer_frame, ptr %frame, i32 0, i32 1\n"
        "  br label %loop\n"
        "loop:\n"
        "  %capture = load ptr, ptr %head\n"
        "  %present = icmp ne ptr %capture, null\n"
        "  br i1 %present, label %drop, label %done\n"
        "drop:\n"
        "  %next = load ptr, ptr %capture\n"
        "  store ptr %next, ptr %head\n"
        "  call void @__ocean.dispose_capture(ptr %capture)\n"
        "  br label %loop\n"
        "done:\n"
        "  ret void\n"
        "}\n"
        "define internal void @__ocean.abandon_capture(ptr %frame) {\n"
        "entry:\n"
        "  %initial = load ptr, ptr @__ocean.recovery_head\n"
        "  br label %loop\n"
        "loop:\n"
        "  %context = phi ptr [ %initial, %entry ], [ %previous, %next ]\n"
        "  %present = icmp ne ptr %context, null\n"
        "  br i1 %present, label %check, label %done\n"
        "check:\n"
        "  %owner.slot = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 1\n"
        "  %owner = load ptr, ptr %owner.slot\n"
        "  %match = icmp eq ptr %owner, %frame\n"
        "  br i1 %match, label %inspect, label %next\n"
        "inspect:\n"
        "  %pending = getelementptr %__ocean.recovery, ptr %context, i32 0, i32 4\n"
        "  %capture = load ptr, ptr %pending\n"
        "  %has.capture = icmp ne ptr %capture, null\n"
        "  br i1 %has.capture, label %drop, label %next\n"
        "drop:\n"
        "  store ptr null, ptr %pending\n"
        "  call void @__ocean.dispose_capture(ptr %capture)\n"
        "  br label %next\n"
        "next:\n"
        "  %previous = load ptr, ptr %context\n"
        "  br label %loop\n"
        "done:\n"
        "  ret void\n"
        "}\n"
        , b->out);
}
