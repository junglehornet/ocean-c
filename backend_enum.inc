/* Generic enum storage and payload records share the same concrete type arguments. */
static Kind enumStorageKind(Backend *b, struct expr_enum *v) {
    if (v->operation == ENUM_EXTRACT || v->operation == ENUM_TAG)
        return expressionKind(b, &v->child);
    Kind *arguments = checked((v->nTypeArguments + 1) * sizeof(Kind));
    for (size_t i = 0; i < v->nTypeArguments; i++) arguments[i] = type(b, v->typeArguments[i]);
    Kind result = recordKind(b, &v->declaration->storage, arguments, v->nTypeArguments);
    free(arguments); return result;
}
static Kind enumPayloadKind(Backend *b, struct stmt_enum *v, size_t member, Kind owner) {
    BufferKind *storage = bufferKind(b, owner);
    if (!storage || storage->shape != 5) { fail(b, "enum payload requires a concrete specialization"); return BAD; }
    if (v->forms[member] != 3)
        return recordKind(b, &v->payloads[member], storage->arguments, storage->nArguments);
    TypeBinding *previous = b->typeBindings;
    TypeBinding *bindings = checked((storage->nArguments + 1) * sizeof(TypeBinding));
    for (size_t i = 0; i < storage->nArguments; i++) {
        bindings[i] = (TypeBinding){v->typeParamNames[i].lexeme, storage->arguments[i], b->typeBindings};
        b->typeBindings = &bindings[i];
    }
    Kind result = v->valueTypes[member].nTokens ? type(b, v->valueTypes[member]) : expressionKind(b, v->values[member]);
    b->typeBindings = previous; free(bindings); return result;
}
static Kind enumExpressionKind(Backend *b, struct expr_enum *v) {
    if (v->operation == ENUM_TAG) return INT;
    Kind owner = enumStorageKind(b, v);
    if (v->operation != ENUM_EXTRACT) return owner;
    Kind payload = enumPayloadKind(b, v->declaration, v->member, owner);
    if (v->declaration->forms[v->member] == 3 || (!v->fields && v->declaration->forms[v->member] == 2)) return payload;
    BufferKind *record = bufferKind(b, payload);
    if (!record) return BAD;
    size_t count = v->fields ? v->nFields : record->count;
    if (count == 1) return record->members[v->fields ? v->fields[0] : 0];
    Kind *members = checked((count + 1) * sizeof(Kind));
    for (size_t i = 0; i < count; i++) members[i] = record->members[v->fields ? v->fields[i] : i];
    Kind result = structKind(b, 2, VOID, members, count); free(members); return result;
}
