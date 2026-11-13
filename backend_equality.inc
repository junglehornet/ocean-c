// Aggregate comparisons use typed helpers, never padding bytes or heap addresses.
// Helpers are queued to allow recursive enum/any/container layouts.
static EqualityKind *equalityKind(Backend *b, Kind left, Kind right) {
    for (EqualityKind *entry = b->equalities; entry; entry = entry->next)
        if (entry->left == left && entry->right == right) return entry;
    EqualityKind *entry = checked(sizeof(*entry));
    *entry = (EqualityKind){.left = left, .right = right, .next = b->equalities};
    b->equalities = entry;
    return entry;
}
static Value aggregateEquality(Backend *b, Value left, Value right) {
    equalityKind(b, left.kind, right.kind);
    Value result = temp(b, BOOL);
    fprintf(b->body, "  %s = call i1 @__ocean.equal.%u.%u(%s %s, %s %s)\n", result.operand,
            (unsigned)left.kind, (unsigned)right.kind, irType(b, left.kind), left.operand,
            irType(b, right.kind), right.operand);
    return result;
}
static Value boxedEquality(Backend *b, enum binaryExprType op, Value left, Value right) {
    b->boxedEqualityUsed = 1;
    left = coerce(b, left, SCALAR_ANY); right = coerce(b, right, SCALAR_ANY);
    Value result = temp(b, BOOL);
    fprintf(b->body, "  %s = call i1 @__ocean.any.equal({ ptr, i64 } %s, { ptr, i64 } %s)\n",
            result.operand, left.operand, right.operand);
    if (op == EXPR_NOT_EQUALS) {
        Value inverted = temp(b, BOOL);
        fprintf(b->body, "  %s = xor i1 %s, true\n", inverted.operand, result.operand); result = inverted;
    }
    return result;
}
static int equalityCollection(BufferKind *kind) {
    return kind && (kind->shape == 4 || !kind->shape);
}
static void emitEqualityBody(Backend *b, EqualityKind *entry) {
    Value left = value(b, entry->left, "%%left"), right = value(b, entry->right, "%%right");
    BufferKind *a = bufferKind(b, left.kind), *c = bufferKind(b, right.kind);
    if ((a && a->map) || (c && c->map)) {
        if (!a || !c || !a->map || !c->map || left.kind != right.kind) {
            fputs("  ret i1 false\n", b->body); b->terminated = 1; return;
        }
        Value al = load(b, mapCountAddress(b, left)), cl = load(b, mapCountAddress(b, right)), same = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq i64 %s, %s\n", same.operand, al.operand, cl.operand);
        unsigned start = ++b->label, check = ++b->label, inspect = ++b->label, probe = ++b->label,
                 compare = ++b->label, advance = ++b->label, unequal = ++b->label, equal = ++b->label;
        branch(b, same, start, unequal); label(b, start);
        Value cursor = slot(b, INT), count = arrayLength(b, left);
        rawStore(b, cursor, value(b, INT, "0")); jump(b, check); label(b, check);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, inspect, equal); label(b, inspect);
        Value entry = elementAddress(b, left, i), occupied = load(b, mapField(b, entry, 0, BOOL));
        branch(b, occupied, probe, advance); label(b, probe);
        Value key = load(b, mapField(b, entry, 1, a->mapKey)), index = mapProbe(b, right, key), valid = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ne i64 %s, -1\n", valid.operand, index.operand);
        unsigned found = ++b->label;
        branch(b, valid, found, unequal); label(b, found);
        Value other = elementAddress(b, right, index), present = load(b, mapField(b, other, 0, BOOL));
        branch(b, present, compare, unequal); label(b, compare);
        Value sameValue = binary(b, EXPR_EQUALITY, load(b, mapField(b, entry, 2, a->mapValue)), load(b, mapField(b, other, 2, c->mapValue)));
        branch(b, sameValue, advance, unequal); label(b, advance);
        Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, check);
        label(b, unequal); fputs("  ret i1 false\n", b->body); b->terminated = 1;
        label(b, equal); fputs("  ret i1 true\n", b->body); b->terminated = 1; return;
    }
    if (equalityCollection(a) && equalityCollection(c)) {
        if (a->element != c->element &&
            !(equalityCollection(bufferKind(b, a->element)) && equalityCollection(bufferKind(b, c->element)))) {
            fputs("  ret i1 false\n", b->body); b->terminated = 1; return;
        }
        Value leftLength = arrayLength(b, left), rightLength = arrayLength(b, right), same = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq i64 %s, %s\n", same.operand, leftLength.operand, rightLength.operand);
        unsigned start = ++b->label, condition = ++b->label, item = ++b->label,
                 advance = ++b->label, unequal = ++b->label, equal = ++b->label;
        branch(b, same, start, unequal); label(b, start);
        Value leftData = arrayData(b, left), rightData = arrayData(b, right), cursor = slot(b, INT);
        rawStore(b, cursor, value(b, INT, "0")); jump(b, condition); label(b, condition);
        Value index = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", more.operand, index.operand, leftLength.operand);
        branch(b, more, item, equal); label(b, item);
        Value leftAddress = temp(b, a->element), rightAddress = temp(b, c->element);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", leftAddress.operand, irType(b, a->element), leftData.operand, index.operand);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", rightAddress.operand, irType(b, c->element), rightData.operand, index.operand);
        Value sameItem = binary(b, EXPR_EQUALITY, load(b, leftAddress), load(b, rightAddress));
        branch(b, sameItem, advance, unequal); label(b, advance);
        Value next = temp(b, INT);
        fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, index.operand);
        rawStore(b, cursor, next); jump(b, condition);
        label(b, unequal); fputs("  ret i1 false\n", b->body); b->terminated = 1;
        label(b, equal); fputs("  ret i1 true\n", b->body); b->terminated = 1;
        return;
    }
    if (a && c && left.kind == right.kind && (a->shape == 2 || a->shape == 5 || a->shape == 17)) {
        unsigned unequal = ++b->label;
        for (size_t i = 0; i < a->count; i++) {
            Value same = binary(b, EXPR_EQUALITY, extract(b, left, i), extract(b, right, i));
            unsigned next = ++b->label;
            branch(b, same, next, unequal); label(b, next);
        }
        fputs("  ret i1 true\n", b->body); b->terminated = 1;
        label(b, unequal); fputs("  ret i1 false\n", b->body); b->terminated = 1;
        return;
    }
    if ((a && (a->shape == 4 || a->shape == 5 || a->shape == 2 || a->shape == 17 || !a->shape)) ||
        (c && (c->shape == 4 || c->shape == 5 || c->shape == 2 || c->shape == 17 || !c->shape))) {
        fputs("  ret i1 false\n", b->body); b->terminated = 1; return;
    }
    Value result = binary(b, EXPR_EQUALITY, left, right);
    fprintf(b->body, "  ret i1 %s\n", result.operand); b->terminated = 1;
}
static void prepareEqualityHelpers(Backend *b) {
    for (;;) {
        if (b->boxedEqualityUsed)
            for (AnyType *type = b->anyTypes; type; type = type->next) equalityKind(b, type->kind, type->kind);
        EqualityKind *entry = b->equalities;
        while (entry && entry->emitted) entry = entry->next;
        if (!entry) break;
        entry->emitted = 1;
        Scope scope = {.parent = b->globalScope};
        b->scope = &scope; b->body = tmpfile(); b->slots = tmpfile(); b->boxRegion = (Value){0}; b->terminated = 0;
        emitEqualityBody(b, entry);
        fprintf(b->out, "define internal i1 @__ocean.equal.%u.%u(%s %%left, %s %%right) {\nentry:\n",
                (unsigned)entry->left, (unsigned)entry->right, irType(b, entry->left), irType(b, entry->right));
        copy(b->slots, b->out); copy(b->body, b->out); fputs("}\n", b->out);
        fclose(b->slots); fclose(b->body); clearScope(&scope); b->scope = NULL;
    }
}
// Decode existing boxes without cloning or registering temporary ownership.
static Value equalityBits(Backend *b, Kind kind, const char *bits) {
    BufferKind *layout = bufferKind(b, kind);
    Value result;
    if (layout && (layout->shape == 4 || layout->shape == 5 || layout->shape == 17 || !layout->shape ||
                   (layout->definition && representationKind(b, kind) == SCALAR_ANY))) {
        result = temp(b, kind);
        fprintf(b->body, "  %s = call %s @__ocean.any.unbox.%u(i64 %s)\n", result.operand, irType(b, kind), (unsigned)kind, bits);
    } else {
        Value storage = slot(b, INT); rawStore(b, storage, value(b, INT, "%s", bits));
        result = load(b, value(b, representationKind(b, kind) == F32 ? FLOAT : kind, "%s", storage.operand));
        if (representationKind(b, kind) == F32) result = convert(b, result, kind);
    }
    return result;
}
static void emitBoxedEquality(Backend *b) {
    if (!b->boxedEqualityUsed) return;
    Scope scope = {.parent = b->globalScope}; b->scope = &scope;
    b->body = tmpfile(); b->slots = tmpfile(); b->terminated = 0;
    fputs("  %left.tag = extractvalue { ptr, i64 } %left, 0\n"
          "  %right.tag = extractvalue { ptr, i64 } %right, 0\n"
          "  %same.type = icmp eq ptr %left.tag, %right.tag\n"
          "  %left.bits = extractvalue { ptr, i64 } %left, 1\n"
          "  %right.bits = extractvalue { ptr, i64 } %right, 1\n", b->body);
    unsigned matching = ++b->label, numeric = ++b->label, unequal = ++b->label;
    branch(b, value(b, BOOL, "%%same.type"), matching, numeric); label(b, matching);
    for (AnyType *type = b->anyTypes; type; type = type->next) {
        Value matches = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq ptr %%left.tag, @__ocean.any.type.%u\n", matches.operand, (unsigned)type->kind);
        unsigned selected = ++b->label, next = ++b->label;
        branch(b, matches, selected, next); label(b, selected);
        Value a = equalityBits(b, type->kind, "%left.bits"), c = equalityBits(b, type->kind, "%right.bits");
        Value result = aggregateEquality(b, a, c);
        fprintf(b->body, "  ret i1 %s\n", result.operand); b->terminated = 1;
        label(b, next);
    }
    jump(b, unequal); label(b, numeric);
    // Preserve existing numeric equality across primitive boxing widths/types.
    for (AnyType *left = b->anyTypes; left; left = left->next) {
        if (definedKind(b, left->kind) || (!integral(b, left->kind) && !floating(b, left->kind))) continue;
        for (AnyType *right = b->anyTypes; right; right = right->next) {
            if (left->kind == right->kind || definedKind(b, right->kind) ||
                (!integral(b, right->kind) && !floating(b, right->kind)) ||
                !runtimeOperator(b, EXPR_EQUALITY, left->kind, right->kind)) continue;
            Value l = temp(b, BOOL), r = temp(b, BOOL), match = temp(b, BOOL);
            fprintf(b->body, "  %s = icmp eq ptr %%left.tag, @__ocean.any.type.%u\n", l.operand, (unsigned)left->kind);
            fprintf(b->body, "  %s = icmp eq ptr %%right.tag, @__ocean.any.type.%u\n", r.operand, (unsigned)right->kind);
            fprintf(b->body, "  %s = and i1 %s, %s\n", match.operand, l.operand, r.operand);
            unsigned selected = ++b->label, next = ++b->label;
            branch(b, match, selected, next); label(b, selected);
            Value a = equalityBits(b, left->kind, "%left.bits"), c = equalityBits(b, right->kind, "%right.bits");
            Value result = binary(b, EXPR_EQUALITY, a, c);
            fprintf(b->body, "  ret i1 %s\n", result.operand); b->terminated = 1; label(b, next);
        }
    }
    jump(b, unequal); label(b, unequal); fputs("  ret i1 false\n", b->body);
    fputs("define internal i1 @__ocean.any.equal({ ptr, i64 } %left, { ptr, i64 } %right) {\nentry:\n", b->out);
    copy(b->slots, b->out); copy(b->body, b->out); fputs("}\n", b->out);
    fclose(b->slots); fclose(b->body); clearScope(&scope); b->scope = NULL;
}

static Value stringComparison(Backend *b, enum binaryExprType op, Value left, Value right) {
    Value leftLength = arrayBytes(b, left), rightLength = arrayBytes(b, right);
    Value shorter = temp(b, BOOL), count = temp(b, INT), compared = slot(b, I32), present = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", shorter.operand, leftLength.operand, rightLength.operand);
    fprintf(b->body, "  %s = select i1 %s, i64 %s, i64 %s\n", count.operand, shorter.operand, leftLength.operand, rightLength.operand);
    rawStore(b, compared, value(b, I32, "0"));
    fprintf(b->body, "  %s = icmp ne i64 %s, 0\n", present.operand, count.operand);
    unsigned compare = ++b->label, done = ++b->label;
    branch(b, present, compare, done); label(b, compare);
    Value dataLeft = arrayData(b, left), dataRight = arrayData(b, right), difference = temp(b, I32);
    fprintf(b->body, "  %s = call i32 %s(ptr %s, ptr %s, i64 %s)\n",
        difference.operand, libcSymbol(b, "memcmp"), dataLeft.operand, dataRight.operand, count.operand);
    rawStore(b, compared, difference); jump(b, done); label(b, done);
    Value comparison = load(b, compared), prefixSame = temp(b, BOOL), prefixResult = temp(b, BOOL), lengthResult = temp(b, BOOL), result = temp(b, BOOL);
    const char *prefix = op == EXPR_EQUALITY ? "eq" : op == EXPR_NOT_EQUALS ? "ne" : op == EXPR_LESS_THAN ? "slt" : op == EXPR_GREATER_THAN ? "sgt" : op == EXPR_LESS_EQUALS ? "sle" : "sge";
    const char *length = op == EXPR_EQUALITY ? "eq" : op == EXPR_NOT_EQUALS ? "ne" : op == EXPR_LESS_THAN ? "ult" : op == EXPR_GREATER_THAN ? "ugt" : op == EXPR_LESS_EQUALS ? "ule" : "uge";
    fprintf(b->body, "  %s = icmp eq i32 %s, 0\n", prefixSame.operand, comparison.operand);
    fprintf(b->body, "  %s = icmp %s i32 %s, 0\n", prefixResult.operand, prefix, comparison.operand);
    fprintf(b->body, "  %s = icmp %s i64 %s, %s\n", lengthResult.operand, length, leftLength.operand, rightLength.operand);
    fprintf(b->body, "  %s = select i1 %s, i1 %s, i1 %s\n", result.operand, prefixSame.operand, lengthResult.operand, prefixResult.operand);
    b->stringComparisonUsed = 1;
    return result;
}

/* Dynamic keys use the same structural hash as concrete keys. Primitive boxed
 * numbers normalize to double because boxed equality compares numeric widths. */
static void emitMapHash(Backend *b) {
    if (!b->mapHashUsed) return;
    Scope scope = {.parent = b->globalScope}; b->scope = &scope;
    b->body = tmpfile(); b->slots = tmpfile(); b->terminated = 0;
    fputs("  %tag = extractvalue { ptr, i64 } %key, 0\n"
          "  %bits = extractvalue { ptr, i64 } %key, 1\n", b->body);
    for (AnyType *type = b->anyTypes; type; type = type->next) {
        Value matches = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp eq ptr %%tag, @__ocean.any.type.%u\n", matches.operand, (unsigned)type->kind);
        unsigned selected = ++b->label, next = ++b->label;
        branch(b, matches, selected, next); label(b, selected);
        Value key = equalityBits(b, type->kind, "%bits");
        if (!definedKind(b, key.kind) && (integral(b, key.kind) || floating(b, key.kind))) key = convert(b, key, FLOAT);
        Value hash = mapHash(b, key);
        fprintf(b->body, "  ret i64 %s\n", hash.operand); b->terminated = 1;
        label(b, next);
    }
    fputs("  ret i64 0\n", b->body); b->terminated = 1;
    fputs("define internal i64 @__ocean.map.hash({ ptr, i64 } %key) {\nentry:\n", b->out);
    copy(b->slots, b->out); copy(b->body, b->out); fputs("}\n", b->out);
    fclose(b->slots); fclose(b->body); clearScope(&scope); b->scope = NULL;
}
