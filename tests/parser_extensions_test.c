#include "../parser.c"
#include "../scanner.h"
#include <assert.h>

static size_t checks;
static parseResult parse(const char *source) {
    tokenlist *tokens = scanTokens((char*)source, "parser-extensions");
    parseResult result = parseTokens(tokens);
    free(tokens);
    return result;
}
static parseResult valid(const char *source) {
    parseResult result = parse(source);
    if (result.hasError) fprintf(stderr, "Unexpected rejection: %s\n", source);
    assert(!result.hasError && result.nStatements == 1);
    checks++;
    return result;
}
static void invalid(const char *source) {
    if (!parse(source).hasError) {
        fprintf(stderr, "Unexpected acceptance: %s\n", source);
        abort();
    }
    checks++;
}
static expr expression(const char *source) {
    parseResult result = valid(source);
    stmt statement = result.statements[0];
    if (statement.type == STMT_VAR) {
        struct stmt_var *variable = statement.value;
        assert(variable->nInitValues == 1);
        return variable->initValues[0];
    }
    assert(statement.type == STMT_EXPR);
    return *(expr*)statement.value;
}
static struct expr_instantiation *instance(expr value, size_t count) {
    assert(value.type == EXPR_TYPE_INSTANTIATION);
    struct expr_instantiation *node = value.underlying;
    assert(node->nTypeArguments == count);
    return node;
}
static struct stmt_switch *switchNode(const char *source) {
    parseResult result = valid(source);
    stmt statement = result.statements[0];
    if (statement.type == STMT_FUNC) {
        struct stmt_func *function = statement.value;
        assert(function->body.nStmts == 1);
        statement = function->body.stmts[0];
    }
    assert(statement.type == STMT_SWITCH);
    return statement.value;
}
static void truncated(const char *source) {
    tokenlist *tokens = scanTokens((char*)source, "extension-prefix");
    for (size_t n = 0; n < (size_t)tokens->top; n++) {
        tokenlist prefix = {0};
        init_tokenlist(&prefix);
        for (size_t i = 0; i < n; i++) push_tokenlist(&prefix, tokens->arr[i]);
        push_tokenlist(&prefix, tokens->arr[tokens->top - 1]);
        parseTokens(&prefix);
        checks++;
    }
    free_tokenlist(tokens);
    free(tokens);
}
int main(void) {
    parseResult defaults = valid("struct Default<T, U> { first T; second U; }");
    struct stmt_struct *defaultStruct = defaults.statements[0].value;
    assert(defaultStruct->nTypeParams == 2 && defaultStruct->typeParamConstraints[0].nTokens == 1 &&
           defaultStruct->typeParamConstraints[0].tokens[0].type == ANY &&
           defaultStruct->typeParamConstraints[1].tokens[0].type == ANY);
    valid("interface Default<T> { Get() T; }");
    expression("callback := func<T>(value T) T { return value; };");
    valid("struct Node<T any> { previous *Node<T>; value T; next *Node<T>; }");
    valid("type Record struct { x, y int; callback func(value int) int; child struct { value int; }; };");
    invalid("struct Old { *Node value; }");
    invalid("type Old struct { []int values; };");
    invalid("struct Missing { value; }");
    invalid("struct Duplicate { x, x int; }");
    invalid("type Duplicate struct { x int; x string; };");
    parseResult r = valid("struct Box<T any> { value T; }");
    struct stmt_struct *box = r.statements[0].value;
    assert(box->nTypeParams == 1 && box->nFields == 1);
    assert(!strcmp(box->typeParamNames[0].lexeme, "T"));
    assert(box->typeParamConstraints[0].tokens[0].type == ANY);
    r = valid("interface Reader<T any> { Read() T; }");
    struct stmt_interface *reader = r.statements[0].value;
    assert(reader->nTypeParams == 1 && reader->nMethods == 1);
    r = valid("func identity<T any>(value T) T { return value; }");
    struct stmt_func *function = r.statements[0].value;
    assert(function->nTypeParams == 1 && function->nParams == 1 && function->nResults == 1);
    r = valid("struct Pair<T, U any, V ~int | ~string,> { first Box<Box<T>>; second U; }");
    box = r.statements[0].value;
    assert(box->nTypeParams == 3 && box->typeParamConstraints[2].nTokens == 5);
    assert(box->fieldTypes[0].tokens[box->fieldTypes[0].nTokens - 1].type == BITSHIFT_RIGHT);
    r = valid("type Nested Box<Box<int>>;");
    struct stmt_type *type = r.statements[0].value;
    assert(type->underlying.nTokens == 6 && type->underlying.tokens[5].type == BITSHIFT_RIGHT);
    valid("type Deep Box<Box<Box<int>>>;");
    valid("type Mixed Box<Box[int], Pair<int, []string>>;");
    valid("interface Derived<T Box<int>> { Base<Box<T>>; Read(value Box<T>) Box<T>; }");
    valid("func constrained<T Box<Box<int>>>(value T) T { return value; }");
    r = valid("struct Constrained<T Box<int>> { value T; }");
    box = r.statements[0].value;
    assert(box->typeParamConstraints[0].nTokens == 4 &&
           box->typeParamConstraints[0].tokens[3].type == GREATER_THAN);
    expr e = expression("make<Box<int>, []string>(x);");
    assert(e.type == EXPR_TYPE_CALL);
    struct expr_instantiation *inst = instance(((struct expr_call*)e.underlying)->func, 2);
    assert(inst->typeArguments[0].nTokens == 4);
    e = expression("make<Box<Box<int>>>(x);");
    inst = instance(((struct expr_call*)e.underlying)->func, 1);
    assert(inst->typeArguments[0].nTokens == 6);
    e = expression("make<Box<int>>(x);");
    inst = instance(((struct expr_call*)e.underlying)->func, 1);
    assert(inst->typeArguments[0].nTokens == 4 && inst->typeArguments[0].tokens[3].type == GREATER_THAN);
    e = expression("Box<int>{value: 1};");
    assert(e.type == EXPR_TYPE_COMPOSITE);
    instance(((struct expr_composite*)e.underlying)->type, 1);
    e = expression("a < b > c;"); assert(e.type == EXPR_TYPE_BINARY);
    e = expression("a >> b;");
    assert(e.type == EXPR_TYPE_BINARY && ((struct expr_binary*)e.underlying)->operation == EXPR_BITSHIFT_RIGHT);
    valid("type Dimension int[N >> 1];");
    invalid("struct Box<> {}"); valid("struct Box<T> {}");
    invalid("struct Box<T: any> {}");
    invalid("interface Reader<T: any> {}");
    invalid("func identity<T: any>(value T) T { return value; }");
    invalid("x := func<T: any>() {};"); invalid("struct Box<T any, T any> {}");
    invalid("struct Box<index any> {}"); invalid("struct Box<T any] {}");
    invalid("type Missing Box<int;"); invalid("type Extra Box<int>>;");
    invalid("type Empty Box<>;"); invalid("type Broken Box<int,,string>;");
    invalid("type Broken Box<Box<int>;");

    e = expression("identity := func[T any](value T) T { return value; };");
    assert(e.type == EXPR_TYPE_FUNCTION);
    function = e.underlying;
    assert(function->nTypeParams == 1 && function->nParams == 1 && function->nResults == 1);
    e = expression("identity := func<T any>(value T) T { return value; };");
    assert(e.type == EXPR_TYPE_FUNCTION && ((struct stmt_func*)e.underlying)->nTypeParams == 1);
    invalid("x := func[]() {};"); invalid("x := func[T any, T any]() {};");
    invalid("x := func[T any](x ...T, y T) {};");
    invalid("struct Bad { +[T any]() {} }");

    inst = instance(expression("identity[int];"), 1);
    assert(inst->parent.type == EXPR_TYPE_IDENTIFIER);
    inst = instance(expression("callback := pkg.identity[int];"), 1);
    assert(inst->parent.type == EXPR_TYPE_DOT);
    instance(expression("identity<int>;"), 1);
    instance(expression("identity<int,>;"), 1);
    instance(expression("identity[Box<int>, []string,];"), 2);
    instance(expression("a[i];"), 1); // Type/value ambiguity is retained for name resolution.
    e = expression("a[i + 1];"); assert(e.type == EXPR_TYPE_INDEX);
    e = expression("a[0];"); assert(e.type == EXPR_TYPE_INDEX);
    invalid("identity[int,,string];"); invalid("identity[[]];");

    e = expression("self[];");
    assert(e.type == EXPR_TYPE_INDEX && ((struct expr_index*)e.underlying)->index.type == EXPR_TYPE_NULL);
    e = expression("self[].length;");
    assert(e.type == EXPR_TYPE_DOT && ((struct expr_dot*)e.underlying)->parent.type == EXPR_TYPE_INDEX);
    e = expression("self[][0];");
    assert(e.type == EXPR_TYPE_INDEX && ((struct expr_index*)e.underlying)->parent.type == EXPR_TYPE_INDEX);
    valid("struct Values { index data []int; +() { use(self[].length); } }");
    invalid("self[;"); invalid("self[,];");

    struct stmt_switch *sw = switchNode("func f() { switch (x := next(); x) { case 1: return; default: return; } }");
    assert(sw->initializer.type == STMT_VAR && !sw->isTypeSwitch && sw->nCases == 2);
    assert(sw->value->type == EXPR_TYPE_IDENTIFIER && sw->cases[0].nValues == 1);
    sw = switchNode("switch (prepare(); value) { case 1: break; }");
    assert(sw->initializer.type == STMT_EXPR && sw->value && !sw->isTypeSwitch);
    sw = switchNode("switch (x := next();) { case x > 0: break; }");
    assert(sw->initializer.type == STMT_VAR && !sw->value);
    sw = switchNode("switch { default: break; }"); assert(!sw->value && !sw->isTypeSwitch);
    invalid("switch (x := next()) {}"); invalid("switch (next(); x; y) {}");
    invalid("switch (; x) {}");

    sw = switchNode("func f(value any) { switch (item := value.(type)) { case int: use(item); default: return; } }");
    assert(sw->isTypeSwitch && !strcmp(sw->binding.lexeme, "item") && sw->nCases == 2);
    assert(sw->cases[0].nTypes == 1 && sw->cases[0].nValues == 0);
    assert(!strcmp(sw->cases[0].types[0].tokens[0].lexeme, "int"));
    assert(sw->cases[1].nTypes == 0 && sw->cases[1].nValues == 0);
    sw = switchNode("func f(value any) { switch (value.(type)) { case string: return; } }");
    assert(sw->isTypeSwitch && sw->binding.type == EOF_TOKEN);
    sw = switchNode("switch (prepare(); item := value.(type)) { case []int, Box<Box<int>>, pkg.Item*: use(item); default: break; }");
    assert(sw->isTypeSwitch && sw->initializer.type == STMT_EXPR && sw->cases[0].nTypes == 3);
    assert(sw->cases[0].types[1].nTokens == 6);
    e = expression("value.(Box<int>).field;"); assert(e.type == EXPR_TYPE_DOT);
    assert(((struct expr_dot*)e.underlying)->parent.type == EXPR_TYPE_ASSERTION);
    sw = switchNode("switch (value.(type)) { case func(), func() int: break; }");
    assert(sw->cases[0].nTypes == 2 && sw->cases[0].types[0].nTokens == 3);
    valid("callback: func() = noop;");
    invalid("value.(type);"); invalid("value.();");
    invalid("switch (value.(type) + 1) {}");
    invalid("switch (f(value.(type))) {}");
    invalid("switch (a, b := value.(type)) {}");
    invalid("switch (x: int = value.(type)) {}");
    invalid("switch (x = value.(type)) {}");
    invalid("switch (x := value.(type); x) {}");
    invalid("switch (value.(type)) { case 1: break; }");
    invalid("switch (value.(type)) { case int,: break; }");
    invalid("switch (value.(type)) { case int: fallthrough; default: break; }");
    invalid("switch (value.(type)) { default: break; default: break; }");
    valid("switch (fn := func() { switch (x.(type)) { case int: break; } }; y) { default: break; }");
    valid("switch (x.(type)) { case int: switch (y) { case 1: fallthrough; default: break; } }");

    r = valid("func accepts(func() int) {}");
    function = r.statements[0].value;
    assert(function->nParams == 1 && function->paramNames[0].type == EOF_TOKEN && function->paramTypeNames[0].nTokens == 4);
    r = valid("func accepts(callback func() ()) {}");
    function = r.statements[0].value;
    assert(!strcmp(function->paramNames[0].lexeme, "callback"));
    valid("func accepts(callback func() int) {}");
    valid("func accepts(callback func() (int)) {}");
    invalid("package main\nfunc main() {}");
    invalid("func f() {\n x := 1\n use(x)\n }");
    invalid("func f() { for (;;) { break\n } }");

    char deep[6 * PARSER_MAX_RECURSION_DEPTH + 64] = "type TooDeep ";
    for (size_t i = 0; i < PARSER_MAX_RECURSION_DEPTH; i++) strcat(deep, "Box<");
    strcat(deep, "int");
    for (size_t i = 0; i < PARSER_MAX_RECURSION_DEPTH; i++) strcat(deep, ">");
    strcat(deep, ";");
    invalid(deep);

    truncated("struct Box<T Box<Box<int>>> { data Box<T>; +() { use(self[].length); } }");
    truncated("callback := func<T any>(x T) T { return identity<Box<T>>(x); };");
    truncated("switch (init(); item := value.(type)) { case Box<Box<int>>, []string: use(item); default: break; }");
    parseResult recovery = parse("type Broken Box<int>>; func recovered() {}");
    assert(recovery.hasError && recovery.nStatements == 1 && recovery.statements[0].type == STMT_FUNC);
    printf("%zu parser extension checks passed\n", checks);
}
