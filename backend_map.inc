/* Maps use open addressing with linear probing. The ordinary dynamic collection
 * header owns buckets; its second word records live entries rather than capacity.
 * Each bucket is {occupied, key, value}, so collection copies and destruction
 * recursively own keys and values, including boxes, strings, and nested maps. */
static Value mapField(Backend *b, Value entry, unsigned field, Kind kind) {
    Value result = temp(b, kind);
    result.ownedBox = representationKind(b, kind) == SCALAR_ANY;
    fprintf(b->body, "  %s = getelementptr %s, ptr %s, i32 0, i32 %u\n",
            result.operand, irType(b, entry.kind), entry.operand, field);
    return result;
}
static Value mapHash(Backend *b, Value key) {
    BufferKind *t = bufferKind(b, key.kind);
    if (representationKind(b, key.kind) == SCALAR_ANY) {
        b->mapHashUsed = 1;
        Value result = temp(b, INT);
        fprintf(b->body, "  %s = call i64 @__ocean.map.hash({ ptr, i64 } %s)\n", result.operand, key.operand);
        return result;
    }
    if (integral(b, key.kind)) return convert(b, key, INT);
    if (floating(b, key.kind)) {
        Value number = convert(b, key, FLOAT), zero = temp(b, BOOL), normalized = temp(b, FLOAT), bits = temp(b, INT);
        fprintf(b->body, "  %s = fcmp oeq double %s, 0.000000e+00\n", zero.operand, number.operand);
        fprintf(b->body, "  %s = select i1 %s, double 0.000000e+00, double %s\n", normalized.operand, zero.operand, number.operand);
        fprintf(b->body, "  %s = bitcast double %s to i64\n", bits.operand, normalized.operand);
        return bits;
    }
    if (key.kind == RAW_POINTER || (t && (t->shape == 3 || t->shape == 6))) {
        Value result = temp(b, INT);
        fprintf(b->body, "  %s = ptrtoint ptr %s to i64\n", result.operand, key.operand);
        return result;
    }
    if (t && !t->map && (t->shape == 4 || !t->shape)) {
        Value count = arrayLength(b, key), data = arrayData(b, key);
        Value cursor = slot(b, INT), hash = slot(b, INT);
        rawStore(b, cursor, value(b, INT, "0")); rawStore(b, hash, value(b, INT, "5381"));
        unsigned check = ++b->label, body = ++b->label, end = ++b->label;
        jump(b, check); label(b, check);
        Value i = load(b, cursor), more = temp(b, BOOL);
        fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", more.operand, i.operand, count.operand);
        branch(b, more, body, end); label(b, body);
        Value address = temp(b, t->element), byte, next = temp(b, INT), product = temp(b, INT), sum = temp(b, INT);
        fprintf(b->body, "  %s = getelementptr %s, ptr %s, i64 %s\n", address.operand, irType(b, t->element), data.operand, i.operand);
        Value character = load(b, address), h = load(b, hash);
        byte = mapHash(b, character);
        fprintf(b->body, "  %s = mul i64 %s, 33\n", product.operand, h.operand);
        fprintf(b->body, "  %s = add i64 %s, %s\n", sum.operand, product.operand, byte.operand);
        rawStore(b, hash, sum);
        fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
        rawStore(b, cursor, next); jump(b, check); label(b, end);
        return load(b, hash);
    }
    if (t && (t->shape == 2 || t->shape == 5 || t->shape == 17)) {
        Value hash = value(b, INT, "5381");
        for (size_t i = 0; i < t->count; i++) {
            Value item = mapHash(b, extract(b, key, i)), product = temp(b, INT), sum = temp(b, INT);
            fprintf(b->body, "  %s = mul i64 %s, 33\n", product.operand, hash.operand);
            fprintf(b->body, "  %s = add i64 %s, %s\n", sum.operand, product.operand, item.operand);
            hash = sum;
        }
        return hash;
    }
    /* Maps and enums share a bucket seed. Equality resolves
     * collisions without hashing padding, addresses, or erased dynamic types. */
    return value(b, INT, "0");
}
/* Find the matching bucket or first empty bucket; -1 denotes an empty table. */
static Value mapProbe(Backend *b, Value map, Value key) {
    BufferKind *t = bufferKind(b, map.kind);
    Value count = arrayLength(b, map), result = slot(b, INT), present = temp(b, BOOL);
    rawStore(b, result, value(b, INT, "-1"));
    fprintf(b->body, "  %s = icmp ne i64 %s, 0\n", present.operand, count.operand);
    unsigned start = ++b->label, check = ++b->label, compare = ++b->label, advance = ++b->label, end = ++b->label;
    branch(b, present, start, end); label(b, start);
    Value hash = mapHash(b, key), initial = temp(b, INT);
    /* Avalanche high bits before choosing a power-of-two bucket count. */
    for (unsigned round = 0; round < 2; round++) {
        Value shifted = temp(b, INT), mixed = temp(b, INT), product = temp(b, INT);
        fprintf(b->body, "  %s = lshr i64 %s, 33\n", shifted.operand, hash.operand);
        fprintf(b->body, "  %s = xor i64 %s, %s\n", mixed.operand, hash.operand, shifted.operand);
        fprintf(b->body, "  %s = mul i64 %s, %s\n", product.operand, mixed.operand,
                round ? "-4265267296055464877" : "-49064778989728563");
        hash = product;
    }
    Value shifted = temp(b, INT), mixed = temp(b, INT);
    fprintf(b->body, "  %s = lshr i64 %s, 33\n", shifted.operand, hash.operand);
    fprintf(b->body, "  %s = xor i64 %s, %s\n", mixed.operand, hash.operand, shifted.operand);
    fprintf(b->body, "  %s = urem i64 %s, %s\n", initial.operand, mixed.operand, count.operand);
    rawStore(b, result, initial); jump(b, check); label(b, check);
    Value index = load(b, result), entry = elementAddress(b, map, index);
    Value occupied = load(b, mapField(b, entry, 0, BOOL));
    branch(b, occupied, compare, end); label(b, compare);
    Value saved = load(b, mapField(b, entry, 1, t->mapKey));
    Value equal = binary(b, EXPR_EQUALITY, saved, key);
    branch(b, equal, end, advance); label(b, advance);
    Value next = temp(b, INT), wrapped = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, index.operand);
    fprintf(b->body, "  %s = urem i64 %s, %s\n", wrapped.operand, next.operand, count.operand);
    rawStore(b, result, wrapped); jump(b, check); label(b, end);
    return load(b, result);
}
static Value mapRead(Backend *b, Value map, Value key, Value *found) {
    BufferKind *t = bufferKind(b, map.kind);
    checkElementRequirement(b, key, t->mapKeyRequirement);
    Value index = mapProbe(b, map, key), result = slot(b, t->mapValue), ok = slot(b, BOOL), valid = temp(b, BOOL);
    rawStore(b, result, emptyValue(b, t->mapValue)); rawStore(b, ok, value(b, BOOL, "false"));
    fprintf(b->body, "  %s = icmp ne i64 %s, -1\n", valid.operand, index.operand);
    unsigned inspect = ++b->label, read = ++b->label, end = ++b->label;
    branch(b, valid, inspect, end); label(b, inspect);
    Value entry = elementAddress(b, map, index), occupied = load(b, mapField(b, entry, 0, BOOL));
    branch(b, occupied, read, end); label(b, read);
    rawStore(b, result, copyValue(b, load(b, mapField(b, entry, 2, t->mapValue)), 0));
    rawStore(b, ok, value(b, BOOL, "true")); jump(b, end); label(b, end);
    *found = load(b, ok); return load(b, result);
}
/* Single-value indexing requires presence; two-value indexing uses mapRead's
 * success flag directly. Use the ordinary panic runtime so defer/recover work. */
static void requireMapKey(Backend *b, Value found, token location) {
    unsigned good = ++b->label, bad = ++b->label;
    branch(b, found, good, bad); label(b, bad);
    const char *message = "map key does not exist";
    Value data = moduleString(b, message);
    location = diagnosticLocation(b, location);
    Value filename = moduleString(b, location.filename);
    char *text = sourceSnippet(b, location);
    Value snippet = moduleString(b, text);
    free(text);
    b->boundsUsed = 1;
    fprintf(b->body,
            "  call void @__ocean.panic(ptr %s, i64 %zu, ptr %s, i64 %ld, i64 %ld, ptr %s)\n"
            "  unreachable\n", data.operand, strlen(message), filename.operand,
            location.line, location.column, snippet.operand);
    b->terminated = 1; label(b, good);
}
static Value mapWriteAddress(Backend *b, Value binding, Value key) {
    BufferKind *t = bufferKind(b, binding.kind);
    checkElementRequirement(b, key, t->mapKeyRequirement);
    /* Snapshot a key before growing a table that might also own that key. */
    key = copyValue(b, key, 0);
    Value map = load(b, binding), count = arrayLength(b, map), live = load(b, mapCountAddress(b, map));
    Value twice = temp(b, INT), crowded = temp(b, BOOL);
    fprintf(b->body, "  %s = mul i64 %s, 2\n", twice.operand, live.operand);
    fprintf(b->body, "  %s = icmp uge i64 %s, %s\n", crowded.operand, twice.operand, count.operand);
    unsigned grow = ++b->label, ready = ++b->label;
    branch(b, crowded, grow, ready); label(b, grow);
    Value doubled = temp(b, INT), small = temp(b, BOOL), capacity = temp(b, INT);
    fprintf(b->body, "  %s = mul i64 %s, 2\n", doubled.operand, count.operand);
    fprintf(b->body, "  %s = icmp ult i64 %s, 8\n", small.operand, doubled.operand);
    fprintf(b->body, "  %s = select i1 %s, i64 8, i64 %s\n", capacity.operand, small.operand, doubled.operand);
    Value replacement = allocateCollection(b, map.kind, capacity), cursor = slot(b, INT);
    rawStore(b, mapCountAddress(b, replacement), live); rawStore(b, cursor, value(b, INT, "0"));
    unsigned check = ++b->label, inspect = ++b->label, move = ++b->label, advance = ++b->label, done = ++b->label;
    jump(b, check); label(b, check);
    Value i = load(b, cursor), more = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ult i64 %s, %s\n", more.operand, i.operand, count.operand);
    branch(b, more, inspect, done); label(b, inspect);
    Value entry = elementAddress(b, map, i), occupied = load(b, mapField(b, entry, 0, BOOL));
    branch(b, occupied, move, advance); label(b, move);
    Value oldKey = load(b, mapField(b, entry, 1, t->mapKey)), index = mapProbe(b, replacement, oldKey);
    store(b, elementAddress(b, replacement, index), load(b, entry));
    jump(b, advance); label(b, advance);
    Value next = temp(b, INT); fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    rawStore(b, cursor, next); jump(b, check); label(b, done);
    store(b, binding, replacement); jump(b, ready); label(b, ready);
    map = load(b, binding);
    index = mapProbe(b, map, key); entry = elementAddress(b, map, index);
    occupied = load(b, mapField(b, entry, 0, BOOL));
    unsigned insert = ++b->label, finish = ++b->label;
    branch(b, occupied, finish, insert); label(b, insert);
    store(b, mapField(b, entry, 1, t->mapKey), key);
    rawStore(b, mapField(b, entry, 0, BOOL), value(b, BOOL, "true"));
    Value sizeAddress = mapCountAddress(b, map), size = load(b, sizeAddress), added = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", added.operand, size.operand);
    rawStore(b, sizeAddress, added); jump(b, finish); label(b, finish);
    Value result = mapField(b, entry, 2, t->mapValue);
    result.owner = binding.owner; result.throughPointer = binding.throughPointer;
    result.elementRequirement = t->mapValueRequirement;
    return result;
}
/* Deletion closes the probe cluster by reinserting subsequent buckets, moving
 * their ownership rather than cloning it. No tombstones accumulate. */
static void mapDelete(Backend *b, Value map, Value key) {
    BufferKind *t = bufferKind(b, map.kind);
    checkElementRequirement(b, key, t->mapKeyRequirement);
    Value index = mapProbe(b, map, key), valid = temp(b, BOOL);
    fprintf(b->body, "  %s = icmp ne i64 %s, -1\n", valid.operand, index.operand);
    unsigned inspect = ++b->label, remove = ++b->label, check = ++b->label,
             move = ++b->label, end = ++b->label;
    branch(b, valid, inspect, end); label(b, inspect);
    Value entry = elementAddress(b, map, index), occupied = load(b, mapField(b, entry, 0, BOOL));
    branch(b, occupied, remove, end); label(b, remove);
    dropNestedDynamicArrays(b, load(b, entry), 0);
    rawStore(b, entry, emptyValue(b, t->element));
    Value sizeAddress = mapCountAddress(b, map), size = load(b, sizeAddress), decreased = temp(b, INT);
    fprintf(b->body, "  %s = sub i64 %s, 1\n", decreased.operand, size.operand);
    rawStore(b, sizeAddress, decreased);
    Value cursor = slot(b, INT), count = arrayLength(b, map);
    rawStore(b, cursor, index); jump(b, check); label(b, check);
    Value i = load(b, cursor), next = temp(b, INT), wrapped = temp(b, INT);
    fprintf(b->body, "  %s = add i64 %s, 1\n", next.operand, i.operand);
    fprintf(b->body, "  %s = urem i64 %s, %s\n", wrapped.operand, next.operand, count.operand);
    rawStore(b, cursor, wrapped);
    entry = elementAddress(b, map, wrapped);
    occupied = load(b, mapField(b, entry, 0, BOOL));
    branch(b, occupied, move, end); label(b, move);
    Value saved = load(b, entry), savedKey = extract(b, saved, 1);
    rawStore(b, entry, emptyValue(b, t->element));
    Value destination = mapProbe(b, map, savedKey);
    rawStore(b, elementAddress(b, map, destination), saved);
    jump(b, check); label(b, end);
}
