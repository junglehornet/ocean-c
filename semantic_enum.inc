/* Enum types retain nominal identity; payload records are private compiler layouts. */
static void prepareEnum(struct stmt_enum *v) {
    if (v->prepared) return;
    v->prepared = 1;
    v->storage = (struct stmt_struct){.name = v->name, .enumStorage = 1, .enumeration = v, .indexField = {.type = EOF_TOKEN},
        .nFields = 2, .nVarParams = 2, .nTypeParams = v->nTypeParams,
        .typeParamNames = v->typeParamNames, .typeParamConstraints = v->typeParamConstraints};
    v->storage.fieldNames = calloc(2, sizeof(token));
    v->storage.fieldTypes = calloc(2, sizeof(typeTokens));
    for (size_t i = 0; i < 2; i++) {
        v->storage.fieldNames[i] = v->name;
        v->storage.fieldNames[i].lexeme = i ? "__payload" : "__tag";
        token *t = malloc(sizeof(token)); *t = v->name;
        t->type = i ? ANY : IDENTIFIER; t->lexeme = i ? "any" : "int";
        v->storage.fieldTypes[i] = (typeTokens){1, t};
    }
    for (size_t i = 0; i < v->nMembers; i++) {
        size_t size = strlen(v->name.lexeme) + strlen(v->names[i].lexeme) + 32;
        char *name = malloc(size);
        snprintf(name, size, "__enum.%s.%s", v->name.lexeme, v->names[i].lexeme);
        v->payloads[i].name.lexeme = name;
        v->payloads[i].enumPayload = 1;
        v->payloads[i].nTypeParams = v->nTypeParams;
        v->payloads[i].typeParamNames = v->typeParamNames;
        v->payloads[i].typeParamConstraints = v->typeParamConstraints;
    }
}
static Type *enumType(Context *c, struct stmt_enum *v) {
    prepareEnum(v);
    Type *t = allocate(c, sizeof(*t));
    t->name = v->isBuiltin ? (!strcmp(v->name.lexeme, OCEAN_BUILTIN_OPTIONAL_NAME) ? "Optional" : "Result") : v->name.lexeme; t->enumeration = v; t->record = &v->storage;
    return t;
}
static Type *enumPayloadType(Context *c, Type *owner, size_t i) {
    struct stmt_enum *v = owner->enumeration;
    if (v->forms[i] == 3) {
        if (v->valueTypes[i].nTokens) return parameter(c, owner, v->valueTypes[i]);
        Scope scope = {.parent = c->scope}; c->scope = &scope;
        for (size_t j = 0; j < v->nTypeParams; j++)
            declare(c, v->typeParamNames[j], j < owner->nArguments ? owner->arguments[j] : &any, 1);
        Type *result = expression(c, v->values[i]);
        c->scope = scope.parent; return result;
    }
    Type *t = allocate(c, sizeof(*t));
    t->record = &v->payloads[i]; t->name = t->record->name.lexeme;
    t->nArguments = owner->nArguments; t->arguments = owner->arguments;
    return t;
}
static Type *enumMemberType(Context *c, Type *parent, token name) {
    struct stmt_enum *v = parent->enumeration;
    if (!v->isBuiltin && !exportedName(name.lexeme) && sourcePackage(c, name.filename) != sourcePackage(c, v->name.filename)) {
        error(c, name, "enum variant is not exported by its package"); return &unknown;
    }
    for (size_t i = 0; i < v->nMembers; i++) if (!strcmp(v->names[i].lexeme, name.lexeme)) {
        Type *t = allocate(c, sizeof(*t)); *t = *parent; t->enumMember = i + 1; return t;
    }
    error(c, name, "unknown enum variant"); return &unknown;
}
static int enumSelector(Context *c, expr *e) {
    if (e->type == EXPR_TYPE_ENUM) return ((struct expr_enum *)e->underlying)->operation == ENUM_VARIANT;
    if (e->type != EXPR_TYPE_DOT) return 0;
    expr *parent = &((struct expr_dot *)e->underlying)->parent;
    if (parent->type == EXPR_TYPE_INSTANTIATION) parent = &((struct expr_instantiation *)parent->underlying)->parent;
    if (parent->type != EXPR_TYPE_IDENTIFIER) return 0;
    Symbol *s = lookup(c, ((struct expr_identifier *)parent->underlying)->lexeme.lexeme);
    return s && s->isType && s->type->enumeration;
}
static expr enumExpr(enum enumOperation operation, struct stmt_enum *v, size_t i, expr child, token at) {
    struct expr_enum *node = calloc(1, sizeof(*node));
    *node = (struct expr_enum){.operation = operation, .declaration = v, .member = i, .child = child};
    return (expr){.type = EXPR_TYPE_ENUM, .underlying = node, .location = at};
}
static Type *enumValueType(Context *c, Type *source) {
    Type *result = allocate(c, sizeof(*result)); *result = *source; result->enumMember = 0;
    return result;
}
static expr specializedEnumExpr(enum enumOperation op, Type *owner, size_t member, expr child, token at) {
    expr result = enumExpr(op, owner->enumeration, member, child, at);
    struct expr_enum *node = result.underlying;
    node->nTypeArguments = owner->nArguments;
    node->typeArguments = calloc(owner->nArguments + 1, sizeof(typeTokens));
    for (size_t i = 0; i < owner->nArguments; i++) {
        typeTokens span = owner->enumArguments[i];
        node->typeArguments[i] = (typeTokens){span.nTokens, malloc(span.nTokens * sizeof(token))};
        memcpy(node->typeArguments[i].tokens, span.tokens, span.nTokens * sizeof(token));
    }
    return result;
}
static Type *enumNodeType(Context *c, struct expr_enum *node) {
    Type *t = enumType(c, node->declaration);
    t->nArguments = node->nTypeArguments; t->enumArguments = node->typeArguments;
    t->arguments = allocate(c, (t->nArguments + 1) * sizeof(Type *));
    for (size_t i = 0; i < t->nArguments; i++) t->arguments[i] = typeOf(c, node->typeArguments[i]);
    return t;
}
static Type *enumExtractionType(Context *c, struct expr_enum *v, token at) {
    struct stmt_enum *decl = v->declaration;
    Type *source = expression(c, &v->child);
    if (source->enumeration != decl) {
        error(c, at, "enum extraction requires a value of the same enum"); return &unknown;
    }
    if (source->nArguments != decl->nTypeParams) {
        error(c, at, "enum extraction requires a concrete specialization"); return &unknown;
    }
    size_t member = v->member;
    if (!decl->forms[member]) { error(c, at, "unit enum variant has no payload"); return &nothing; }
    if (decl->forms[member] == 3) return enumPayloadType(c, source, member);
    struct stmt_struct *record = &decl->payloads[member];
    if (!v->fields && decl->forms[member] == 2) return enumPayloadType(c, source, member);
    size_t count = v->fields ? v->nFields : record->nFields;
    if (count == 1) return parameter(c, source, record->fieldTypes[v->fields ? v->fields[0] : 0]);
    Type *tuple = allocate(c, sizeof(*tuple)); tuple->name = "tuple"; tuple->nArguments = count;
    tuple->arguments = allocate(c, sizeof(Type *) * count);
    for (size_t i = 0; i < count; i++) tuple->arguments[i] = parameter(c, source, record->fieldTypes[v->fields ? v->fields[i] : i]);
    return tuple;
}
/* A record pattern maps binding names to fields; tuple patterns are positional. */
static size_t enumPattern(Context *c, struct stmt_enum *decl, size_t member, expr *pattern,
                          token **names, size_t **fields) {
    *names = NULL; *fields = NULL;
    size_t count = 0; expr *items = NULL;
    struct stmt_struct *record = &decl->payloads[member];
    if (pattern->type == EXPR_TYPE_CALL && decl->forms[member] == 1) {
        struct expr_call *p = pattern->underlying; count = p->nParams; items = p->params;
        if (count != record->nFields) {
            error(c, pattern->location, "tuple enum pattern count mismatch; use _ to omit a field"); return 0;
        }
    } else if (pattern->type == EXPR_TYPE_COMPOSITE && decl->forms[member] == 2) {
        struct expr_composite *p = pattern->underlying; count = p->nEntries; items = p->values;
    } else { error(c, pattern->location, "enum pattern does not match variant payload form"); return 0; }
    *names = calloc(count + 1, sizeof(token)); *fields = calloc(count + 1, sizeof(size_t));
    for (size_t i = 0; i < count; i++) {
        expr *binding = &items[i], *field = binding;
        if (decl->forms[member] == 2 && binding->type == EXPR_TYPE_BINARY &&
            ((struct expr_binary *)binding->underlying)->operation == EXPR_ASSIGNMENT) {
            struct expr_binary *assignment = binding->underlying;
            binding = &assignment->left; field = &assignment->right;
        }
        if (binding->type != EXPR_TYPE_IDENTIFIER || field->type != EXPR_TYPE_IDENTIFIER) {
            error(c, pattern->location, "enum pattern bindings and fields must be names"); continue;
        }
        (*names)[i] = ((struct expr_identifier *)binding->underlying)->lexeme;
        size_t index = i;
        if (decl->forms[member] == 2) {
            token name = ((struct expr_identifier *)field->underlying)->lexeme;
            for (index = 0; index < record->nFields; index++) if (!strcmp(record->fieldNames[index].lexeme, name.lexeme)) break;
            if (index == record->nFields) { error(c, name, "unknown enum payload field"); return 0; }
        }
        if (index >= record->nFields) index = 0;
        (*fields)[i] = index;
        for (size_t j = 0; j < i; j++) if ((*fields)[j] == index && decl->forms[member] == 2)
            error(c, (*names)[i], "duplicate enum pattern field");
    }
    return count;
}
static expr enumIdentifier(token name) {
    struct expr_identifier *id = calloc(1, sizeof(*id)); id->lexeme = name;
    return (expr){.type = EXPR_TYPE_IDENTIFIER, .underlying = id, .location = name};
}
static expr enumInteger(long long number, token at) {
    struct expr_literal *lit = calloc(1, sizeof(*lit)); lit->type = LITERAL_TYPE_INT;
    lit->value = malloc(sizeof(long long)); *(long long *)lit->value = number;
    return (expr){.type = EXPR_TYPE_LITERAL, .underlying = lit, .location = at};
}
static void lowerMatch(Context *c, struct stmt_switch *v, Type *subject, token at) {
    struct stmt_enum *decl = subject->enumeration;
    if (!decl) { error(c, at, "match requires an enum value"); v->isMatch = 0; return; }
    static unsigned serial;
    char text[64]; snprintf(text, sizeof(text), "$enum.match.%u", serial++);
    token name = at; name.type = IDENTIFIER; name.lexeme = strdup(text);
    struct stmt_var *init = calloc(1, sizeof(*init)); init->nVars = init->nInitValues = 1;
    init->vars = malloc(sizeof(token)); *init->vars = name; init->initValues = v->value;
    v->initializer = (stmt){.type = STMT_VAR, .value = init, .location = at};
    expr *value = malloc(sizeof(expr)); *value = enumExpr(ENUM_TAG, decl, 0, enumIdentifier(name), at); v->value = value;
    char *seen = calloc(decl->nMembers + 1, 1); int fallback = 0;
    for (size_t i = 0; i < v->nCases; i++) {
        struct switch_case *branch = &v->cases[i]; expr pattern = branch->values[0], *head = &pattern;
        if (pattern.type == EXPR_TYPE_CALL) head = &((struct expr_call *)pattern.underlying)->func;
        if (pattern.type == EXPR_TYPE_COMPOSITE) head = &((struct expr_composite *)pattern.underlying)->type;
        Type *t = expression(c, head);
        if (t->enumeration != decl) { error(c, pattern.location, "match pattern belongs to a different enum"); continue; }
        if (t->nArguments && !compatibleTypes(t, subject)) error(c, pattern.location, "match pattern has different enum type arguments");
        if (!t->enumMember) {
            expr *base = head;
            if (base->type == EXPR_TYPE_INSTANTIATION) base = &((struct expr_instantiation *)base->underlying)->parent;
            Symbol *symbol = base->type == EXPR_TYPE_IDENTIFIER ? lookup(c, ((struct expr_identifier *)base->underlying)->lexeme.lexeme) : NULL;
            if (!symbol || !symbol->isType) error(c, pattern.location, "default match pattern must name the enum type");
            if (head != &pattern) error(c, pattern.location, "default enum pattern cannot bind a payload");
            if (fallback) error(c, pattern.location, "duplicate default enum pattern");
            fallback = 1; branch->nValues = 0; continue;
        }
        size_t member = t->enumMember - 1;
        if (seen[member]) error(c, pattern.location, "duplicate enum match variant");
        seen[member] = 1;
        branch->values[0] = enumInteger(member, pattern.location);
        if (head != &pattern) {
            token *names; size_t *fields;
            size_t count = enumPattern(c, decl, member, &pattern, &names, &fields);
            if (count) {
                expr extracted = enumExpr(ENUM_EXTRACT, decl, member, enumIdentifier(name), pattern.location);
                struct expr_enum *ex = extracted.underlying; ex->fields = fields; ex->nFields = count;
                struct stmt_var *bindings = calloc(1, sizeof(*bindings));
                bindings->vars = names; bindings->nVars = count; bindings->nInitValues = 1;
                bindings->initValues = malloc(sizeof(expr)); *bindings->initValues = extracted;
                stmt *body = malloc((branch->body.nStmts + 1) * sizeof(stmt));
                body[0] = (stmt){.type = STMT_VAR, .value = bindings, .location = pattern.location};
                memcpy(body + 1, branch->body.stmts, branch->body.nStmts * sizeof(stmt));
                branch->body.stmts = body; branch->body.nStmts++;
            }
        } else if (decl->forms[member] == 1 || decl->forms[member] == 2) {
            /* A bare variant intentionally ignores its entire payload. */
        }
    }
    v->exhaustiveMatch = !fallback;
    if (!fallback) for (size_t i = 0; i < decl->nMembers; i++) if (!seen[i]) {
        v->exhaustiveMatch = 0;
        diagnostic(programSource(c->program, at.filename), at, "semantic", "warning", "non-exhaustive enum match: missing %s", decl->names[i].lexeme);
    }
    free(seen); v->isMatch = 0;
}
/* Record extraction is named: declaration/assignment order follows the LHS names. */
static void orderEnumExtraction(Context *c, expr *rhs, token *names, size_t count) {
    if (rhs->type != EXPR_TYPE_ASSERTION) return;
    struct expr_assertion *a = rhs->underlying;
    if (!a->pattern || a->pattern->type != EXPR_TYPE_COMPOSITE) return;
    struct expr_composite *p = a->pattern->underlying;
    if (count != p->nEntries) { error(c, rhs->location, "enum record binding count mismatch"); return; }
    expr *ordered = calloc(count + 1, sizeof(expr));
    for (size_t i = 0; i < count; i++) {
        size_t j;
        for (j = 0; j < count; j++) {
            expr *binding = &p->values[j];
            if (binding->type == EXPR_TYPE_BINARY) binding = &((struct expr_binary *)binding->underlying)->left;
            if (binding->type == EXPR_TYPE_IDENTIFIER && !strcmp(names[i].lexeme, ((struct expr_identifier *)binding->underlying)->lexeme.lexeme)) break;
        }
        if (j == count) { error(c, names[i], "enum record binding name does not match extraction pattern"); free(ordered); return; }
        ordered[i] = p->values[j];
    }
    p->values = ordered;
}
