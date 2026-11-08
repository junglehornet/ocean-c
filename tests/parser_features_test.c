#include "../parser.c"
#include "../scanner.h"
#include <assert.h>

static size_t checks;
static parseResult parse(char *source) {
    tokenlist *tokens = scanTokens(source, "parser-features");
    parseResult result = parseTokens(tokens);
    free(tokens);
    return result;
}
static parseResult valid(char *source, size_t count) {
    parseResult result = parse(source);
    if (result.hasError || result.nStatements != count) fprintf(stderr, "Unexpected parse result: %s\n", source);
    assert(!result.hasError && result.nStatements == count);
    checks++;
    return result;
}
static void invalid(char *source) {
    parseResult result = parse(source);
    if (!result.hasError) fprintf(stderr, "Unexpected acceptance: %s\n", source);
    assert(result.hasError);
    checks++;
}
static expr expression(char *source) {
    parseResult result = valid(source, 1);
    assert(result.statements[0].type == STMT_EXPR);
    return *(expr*)result.statements[0].value;
}
static struct expr_binary *bin(expr value, enum binaryExprType operation) {
    assert(value.type == EXPR_TYPE_BINARY);
    struct expr_binary *node = value.underlying;
    assert(node->operation == operation);
    return node;
}
static void truncatedInputs(char *source) {
    tokenlist *complete = scanTokens(source, "truncated-input");
    token eof = complete->arr[complete->top - 1];
    for (size_t n = 0; n < (size_t)complete->top; n++) {
        tokenlist partial = {0};
        init_tokenlist(&partial);
        for (size_t i = 0; i < n; i++) push_tokenlist(&partial, complete->arr[i]);
        push_tokenlist(&partial, eof);
        // Some prefixes are complete programs. Every prefix must terminate
        // without reading beyond the token array, regardless of validity.
        parseTokens(&partial);
        checks++;
    }
    free_tokenlist(complete);
    free(complete);
}

static void fixture(const char *path, size_t count) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *source = calloc((size_t)length + 1, 1);
    assert(source && fread(source, 1, length, file) == (size_t)length);
    fclose(file);
    valid(source, count);
    free(source);
}

int main(void) {
    expr zeroExpr = expression("zero(int);");
    assert(zeroExpr.type == EXPR_TYPE_ZERO && zeroExpr.location.line == 1 && zeroExpr.location.column == 1);
    struct expr_zero *zero = zeroExpr.underlying;
    assert(zero->type.nTokens == 1 && !strcmp(zero->type.tokens[0].lexeme, "int"));
    zeroExpr = expression("zero(*Box<buffer<T>[4]>);");
    assert(zeroExpr.type == EXPR_TYPE_ZERO && ((struct expr_zero *)zeroExpr.underlying)->type.tokens[0].type == STAR);
    zeroExpr = expression("zero(Box<int>).value;");
    assert(zeroExpr.type == EXPR_TYPE_DOT && ((struct expr_dot *)zeroExpr.underlying)->parent.type == EXPR_TYPE_ZERO);
    zeroExpr = expression("zero(buffer<int>[3])[1];");
    assert(zeroExpr.type == EXPR_TYPE_INDEX && ((struct expr_index *)zeroExpr.underlying)->parent.type == EXPR_TYPE_ZERO);
    invalid("zero();"); invalid("zero(int, bool);"); invalid("zero(1);"); invalid("zero(int + 1);");
    invalid("zero(int;");
    truncatedInputs("zero(*Box<buffer<T>[4]>);");

    expr e = expression("a - b - c;");
    bin(bin(e, EXPR_SUBTRACT)->left, EXPR_SUBTRACT);
    e = expression("a / b * c % d;"); bin(bin(bin(e, EXPR_MODULO)->left, EXPR_MULTIPLY)->left, EXPR_DIVIDE);
    e = expression("a + b * c << d;"); bin(bin(bin(e, EXPR_BITSHIFT_LEFT)->left, EXPR_ADD)->right, EXPR_MULTIPLY);
    e = expression("a << b >> c;"); bin(bin(e, EXPR_BITSHIFT_RIGHT)->left, EXPR_BITSHIFT_LEFT);
    e = expression("a | b | c;"); bin(bin(e, EXPR_BITWISE_OR)->left, EXPR_BITWISE_OR);
    e = expression("a && b && c;"); bin(bin(e, EXPR_LOGICAL_AND)->left, EXPR_LOGICAL_AND);
    e = expression("a || b || c;"); bin(bin(e, EXPR_LOGICAL_OR)->left, EXPR_LOGICAL_OR);
    e = expression("a = b = c;"); bin(bin(e, EXPR_ASSIGNMENT)->right, EXPR_ASSIGNMENT);
    e = expression("a, b, c;"); bin(bin(e, EXPR_COMMA)->left, EXPR_COMMA);
    e = expression("(f)(x);"); assert(e.type == EXPR_TYPE_CALL);
    e = expression("(a)[i].field;"); assert(e.type == EXPR_TYPE_DOT);
    e = expression("a ? b, c : d;"); assert(e.type == EXPR_TYPE_TERNARY);
    valid("package main; func main() {}", 1);
    invalid("package \"legacy\";");
    invalid("package `main`;");
    invalid("package 123;");
    invalid("package ;");
    parseResult r = valid("package demo; import \"io\"; import \"math\" as m; import alias \"path\";", 3);
    assert(!strcmp(r.packageName, "demo"));
    struct stmt_import *imp = r.statements[1].value;
    assert(!strcmp(imp->path, "math") && !strcmp(imp->importedName, "m"));
    valid("import (\"io\", _ \"unused\"; . \"shared\",); func main() {}", 4);
    r = valid("type Count int; type Alias = []pkg.Item;", 2);
    assert(((struct stmt_type*)r.statements[1].value)->isAlias);
    r = valid("struct Point { x, y int; names []string; }", 1);
    struct stmt_struct *point = r.statements[0].value;
    assert(point->nFields == 3 && !strcmp(point->fieldNames[1].lexeme, "y"));
    r = valid("struct Pair[T, U any, V ~int | ~string,] { first T; second U; tag V; next Pair[T, U, V]*; }", 1);
    point = r.statements[0].value;
    assert(point->nTypeParams == 3 && point->nFields == 4);
    assert(!strcmp(point->typeParamNames[0].lexeme, "T") &&
           !strcmp(point->typeParamNames[1].lexeme, "U") &&
           !strcmp(point->typeParamNames[2].lexeme, "V"));
    assert(point->typeParamConstraints[0].nTokens == 1 &&
           point->typeParamConstraints[0].tokens[0].type == ANY &&
           point->typeParamConstraints[1].tokens[0].type == ANY);
    assert(point->typeParamConstraints[2].nTokens == 5 &&
           point->typeParamConstraints[2].tokens[2].type == BITWISE_OR);
    assert(!strcmp(point->fieldTypes[0].tokens[0].lexeme, "T") &&
           point->fieldTypes[3].tokens[1].type == LEFT_BRACKET);
    valid("struct Values[T interface { ~int | ~string; }] { index data []T; }", 1);
    valid("struct Empty[T pkg.Constraint] {}", 1);
    valid("struct Box[T any] { value T; } func (self *Box[T]) Get() T { return self.value; } func use() { box: Box[int]; value := Box[int]{value: 1}; }", 3);
    invalid("struct Bad[] {}"); valid("struct Default[T] {}", 1);
    invalid("struct Bad[T any, T any] {}"); invalid("struct Bad[T, T any] {}");
    invalid("struct Bad[index any] {}"); invalid("struct Bad[T index] {}");
    invalid("struct Bad[T ~] {}"); invalid("struct Bad[T int |] {}");
    invalid("struct Bad[T any {}"); invalid("struct Bad[T any];");
    r = parse("struct Bad[T any] { int; } struct Good[T any] { value T; }");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_STRUCT &&
           ((struct stmt_struct*)r.statements[0].value)->nTypeParams == 1);
    truncatedInputs("struct Pair[T, U any, V ~int | ~string] { first T; second U; tag V; }");
    r = valid("struct Matrix[T any](u64 width, height,) { index underlying T[height][width]; }", 1);
    point = r.statements[0].value;
    assert(point->nTypeParams == 1 && point->nVarParams == 2 && point->nFields == 3);
    assert(!strcmp(point->varParamNames[0].lexeme, "width") &&
           !strcmp(point->varParamNames[1].lexeme, "height"));
    assert(point->varParamTypes[0].nTokens == 1 && point->varParamTypes[1].nTokens == 1 &&
           !strcmp(point->varParamTypes[0].tokens[0].lexeme, "u64") &&
           !strcmp(point->varParamTypes[1].tokens[0].lexeme, "u64"));
    assert(!strcmp(point->indexField.lexeme, "underlying") &&
           point->fieldTypes[2].nTokens == 7 &&
           !strcmp(point->fieldTypes[2].tokens[2].lexeme, "height") &&
           !strcmp(point->fieldTypes[2].tokens[5].lexeme, "width"));
    assert(!strcmp(point->fieldNames[0].lexeme, "width") &&
           !strcmp(point->fieldNames[1].lexeme, "height") &&
           !strcmp(point->fieldNames[2].lexeme, "underlying"));
    assert(point->varParamNames == point->fieldNames && point->varParamTypes == point->fieldTypes);
    r = valid("struct Config(pkg.Size size, []int values, func(x int) (int) transform) { data int[size]; }", 1);
    point = r.statements[0].value;
    assert(point->nTypeParams == 0 && point->nVarParams == 3);
    assert(point->nFields == 4 && !strcmp(point->fieldNames[0].lexeme, "size") &&
           !strcmp(point->fieldNames[1].lexeme, "values") &&
           !strcmp(point->fieldNames[2].lexeme, "transform") &&
           !strcmp(point->fieldNames[3].lexeme, "data"));
    assert(point->varParamTypes[0].nTokens == 3 &&
           point->varParamTypes[0].tokens[1].type == DOT);
    assert(point->varParamTypes[1].nTokens == 3 &&
           point->varParamTypes[1].tokens[0].type == LEFT_BRACKET);
    assert(point->varParamTypes[2].nTokens == 8 &&
           point->varParamTypes[2].tokens[0].type == FUNC &&
           !strcmp(point->varParamNames[2].lexeme, "transform"));
    invalid("struct Bad(int size) { size int; }");
    r = valid("struct View(buffer<int>[] data) { index data; tail int; }", 1);
    point = r.statements[0].value;
    assert(point->nFields == 2 && point->nVarParams == 1 && !strcmp(point->fieldNames[0].lexeme, "data"));
    r = valid("struct Empty() {} struct GenericEmpty[T any]() {} struct Plain {}", 3);
    for (size_t i = 0; i < r.nStatements; i++) {
        point = r.statements[i].value;
        assert(point->nVarParams == 0 && !point->varParamTypes && !point->varParamNames);
    }
    valid("struct Values(int index) { data int[index]; }", 1);
    valid("struct Grouped[T any](Box[T] a, b, int count) { data T[count]; }", 1);
    invalid("struct Bad(int) {}"); invalid("struct Bad(int, string) {}");
    invalid("struct Bad(int _) {}"); invalid("struct Bad(...int values) {}");
    invalid("struct Bad(int width, width) {}");
    invalid("struct Bad(int width, string width) {}");
    invalid("struct Bad[T any](int T) {}");
    invalid("struct Bad(int x = 1) {}"); invalid("struct Bad(int x int y) {}");
    invalid("struct Bad(,int x) {}"); invalid("struct Bad(int x,,) {}");
    invalid("struct Bad(int x {}"); invalid("struct Bad(int x);");
    invalid("struct Bad(int x)[T any] {}");
    valid("struct Named(index x) {}", 1);
    r = parse("struct Bad(int width, width) {} struct Good(int length) { data int[length]; }");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_STRUCT &&
           ((struct stmt_struct*)r.statements[0].value)->nVarParams == 1);
    r = parse("struct Bad[T any](int length) { int; } struct Good {}");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_STRUCT &&
           !strcmp(((struct stmt_struct*)r.statements[0].value)->name.lexeme, "Good"));
    truncatedInputs("struct Matrix[T any](u64 width, height,) { index underlying T[height][width]; }");
    truncatedInputs("struct Config(pkg.Size size, []int values) { data int[size]; }");
    r = valid("enum Color { RED, GREEN = 1 + 2 * 3, BLUE, }", 1);
    struct stmt_enum *colors = r.statements[0].value;
    assert(colors->nMembers == 3 && !colors->values[0] && colors->values[1]);
    r = valid("interface Reader { Read(buffer []byte) (int, error); Close() error; }", 1);
    struct stmt_interface *iface = r.statements[0].value;
    assert(iface->nMethods == 2 && iface->methods[0].nResults == 2);
    r = valid("interface Scalar { ~int | ~string; Reader; pkg.Writer; "
              "Box[int]; Read(buffer []byte) (int, error); func Close(); }", 1);
    iface = r.statements[0].value;
    assert(iface->nConstraints == 4 && iface->nMethods == 2);
    typeTokens unionTerms = iface->constraints[0];
    assert(unionTerms.nTokens == 5 && unionTerms.tokens[0].type == TILDE &&
           !strcmp(unionTerms.tokens[1].lexeme, "int") &&
           unionTerms.tokens[2].type == BITWISE_OR &&
           unionTerms.tokens[3].type == TILDE &&
           !strcmp(unionTerms.tokens[4].lexeme, "string"));
    assert(iface->constraints[1].nTokens == 1 &&
           !strcmp(iface->constraints[1].tokens[0].lexeme, "Reader"));
    assert(iface->constraints[2].nTokens == 3 &&
           iface->constraints[2].tokens[1].type == DOT);
    assert(iface->constraints[3].nTokens == 4 &&
           iface->constraints[3].tokens[3].type == RIGHT_BRACKET);
    assert(!strcmp(iface->methods[0].name.lexeme, "Read") &&
           !iface->methods[0].isExtern && iface->methods[0].nResults == 2);
    valid("interface Empty {} interface Any { any; }", 2);
    r = valid("interface Mapper[T, U any, V ~int | ~string,] { "
              "Base[T]; ~[]V | pkg.Values[V]; Map(value T) U; }", 1);
    iface = r.statements[0].value;
    assert(iface->nTypeParams == 3 && iface->nConstraints == 2 && iface->nMethods == 1);
    assert(!strcmp(iface->typeParamNames[0].lexeme, "T") &&
           !strcmp(iface->typeParamNames[1].lexeme, "U") &&
           !strcmp(iface->typeParamNames[2].lexeme, "V"));
    assert(iface->typeParamConstraints[0].nTokens == 1 &&
           iface->typeParamConstraints[0].tokens[0].type == ANY &&
           iface->typeParamConstraints[2].nTokens == 5);
    assert(iface->constraints[0].nTokens == 4 &&
           !strcmp(iface->constraints[0].tokens[2].lexeme, "T"));
    assert(!strcmp(iface->methods[0].paramTypeNames[0].tokens[0].lexeme, "T") &&
           !strcmp(iface->methods[0].resultTypes[0].tokens[0].lexeme, "U"));
    valid("interface EmptyGeneric[T interface { ~int | ~string; }] {}", 1);
    invalid("interface Bad[] {}"); valid("interface Default[T] {}", 1);
    invalid("interface Bad[T any, T any] {}"); invalid("interface Bad[T, T any] {}");
    invalid("interface Bad[index any] {}"); invalid("interface Bad[T index] {}");
    invalid("interface Bad[T ~] {}"); invalid("interface Bad[T int |] {}");
    invalid("interface Bad[T any {}"); invalid("interface Bad[T any];");
    r = parse("interface Bad[T any] { ~; } interface Good[T any] { M() T; }");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_INTERFACE &&
           ((struct stmt_interface*)r.statements[0].value)->nTypeParams == 1);
    truncatedInputs("interface Mapper[T any, U ~int | ~string] { Base[T]; Map(value T) U; }");

    valid("interface Nested { interface { ~int | ~string; }; "
          "~[]byte | pkg.Bytes; M[T any](T x) T; }", 1);
    invalid("interface Bad { ~; }"); invalid("interface Bad { int |; }");
    invalid("interface Bad { | int; }"); invalid("interface Bad { int || string; }");
    invalid("interface Bad { int }"); invalid("interface Bad { Reader Writer; }");
    invalid("interface Bad { pkg.; }"); invalid("interface Bad { Box[int; }");
    invalid("interface Bad { index; }"); invalid("interface Bad { ~pkg.index; }");
    invalid("interface Bad { Reader; M(); M(); }");
    invalid("interface Bad { Reader; func int; }");
    invalid("interface Bad { Reader; M() {} }");
    r = parse("interface Bad { int |; } func after() {}");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_FUNC);
    truncatedInputs("interface Scalar { Reader; ~int | ~string; Box[int]; M(x int) int; }");

    r = valid("extern int write([]byte bytes); extern void done(void);", 2);
    assert(((struct stmt_func*)r.statements[0].value)->isExtern);
    valid("func f() { defer cleanup(); for (;;) { if (x) break; continue; } }", 1);
    valid("func f() { goto done; done: { return; } }", 1);
    valid("func f() { outer: for (;;) { break outer; } }", 1);
    r = valid("func f() { done: return; again: goto done; call: log(1); "
              "assign: x += 1; inc: x++; decl: x := 1; "
              "first: second: return; empty: ; }", 1);
    struct stmt_func *labeled = r.statements[0].value;
    enum stmtType labelBodies[] = {STMT_RETURN, STMT_GOTO, STMT_EXPR,
        STMT_EXPR, STMT_EXPR, STMT_VAR, STMT_LABEL, STMT_NULL};
    assert(labeled->body.nStmts == sizeof(labelBodies) / sizeof(*labelBodies));
    for (size_t i = 0; i < labeled->body.nStmts; i++) {
        assert(labeled->body.stmts[i].type == STMT_LABEL);
        struct stmt_label *label = labeled->body.stmts[i].value;
        assert(label->body->type == labelBodies[i]);
    }
    struct stmt_label *firstLabel = labeled->body.stmts[6].value;
    struct stmt_label *secondLabel = firstLabel->body->value;
    assert(!strcmp(firstLabel->name.lexeme, "first") &&
           !strcmp(secondLabel->name.lexeme, "second") &&
           secondLabel->body->type == STMT_RETURN);
    valid("func f() { d: defer cleanup(); for (;;) { b: break; c: continue; } }", 1);
    valid("t: type Count int; s: struct Point { x int; } e: enum E { A } i: interface I { M(); } f: func run() {} x: extern void external(void);", 6);
    valid("func f() { i: if (x) return; s: switch { default: return; } }", 1);
    r = valid("x: int; y: pkg.Type = value; z := 1; callback: int(); "
              "a, b: int; ptr: int*;", 6);
    for (size_t i = 0; i < r.nStatements; i++) assert(r.statements[i].type == STMT_VAR);
    invalid("func f() { missing: }"); invalid("missing:");
    invalid("func f() { a: b: }");
    invalid("bad: break;"); invalid("bad: return;");
    invalid("func f() { for (;;) { l: func nested() { b: break; } } }");
    invalid("switch { case 1: l: fallthrough; case 2: break; }");
    truncatedInputs("func f() { first: second: x += 1; done: return; }");
    char labelDepth[4 * PARSER_MAX_RECURSION_DEPTH + 16];
    size_t labelLength = 0;
    for (size_t i = 0; i < PARSER_MAX_RECURSION_DEPTH + 1; i++) {
        memcpy(labelDepth + labelLength, "l: ", 3);
        labelLength += 3;
    }
    strcpy(labelDepth + labelLength, ";");
    invalid(labelDepth);

    r = valid("func f(x int) { switch (x) { case 1, 2: x++; fallthrough; case 3: break; default: return; } }", 1);
    struct stmt_func *f = r.statements[0].value;
    struct stmt_switch *sw = f->body.stmts[0].value;
    assert(sw->nCases == 3 && sw->cases[0].nValues == 2 && sw->cases[2].nValues == 0);
    valid("switch { case a < b: x++; default: x--; }", 1);
    valid("x : map[string][]int;", 1);
    invalid("package ;"); invalid("package a; package b;");
    invalid("func f() {} package late;"); invalid("import \"\";");
    invalid("import \"io\" as ;"); invalid("import a \"io\" as b;");
    invalid("import (\"io\" \"math\");");
    invalid("struct Point { x int; x string; }"); invalid("struct Point { int; }");
    invalid("enum E { A, A }"); invalid("type X = ;"); invalid("x : int garbage;");
    valid("extern i32 abs(i32 value); func main() int { return extern.abs(-3); }", 2);
    valid("extern void notify(); func main() { extern.notify(); }", 2);
    invalid("func main() { extern i32 abs(i32 value); }");
    invalid("func main() { extern.(); }");
    invalid("interface I { M(); M(); }"); invalid("extern func f() {}");
    invalid("break;"); invalid("continue;"); invalid("goto label;"); invalid("return;");
    invalid("func f() { defer x + 1; }"); invalid("func f() { goto; }");
    invalid("switch { default: break; default: break; }");
    invalid("switch { case 1: fallthrough; }"); invalid("switch { case : break; }");
    invalid("func f() { for (;;) { func nested() { break; } } }");
    invalid("a.42;"); invalid("a ~= b;"); invalid("func f() { if (x := 1) {} }");
    r = parse("func broken(x int y int) {} func good() {}");
    assert(r.hasError && r.nStatements == 1 && !strcmp(((struct stmt_func*)r.statements[0].value)->name.lexeme, "good")); checks++;
    r = parse("func broken() { +; } func good() {}");
    assert(r.hasError && r.nStatements == 1); checks++;
    e = expression("a &= b |= c;"); bin(bin(e, EXPR_BITWISE_AND_EQUALS)->right, EXPR_BITWISE_OR_EQUALS);
    r = valid("func (self *Point) Move(x int) int { return x; }", 1);
    f = r.statements[0].value;
    assert(!strcmp(f->receiverName.lexeme, "self") && f->receiverType.nTokens == 2);
    invalid("func (self *Point) Move(self int) {}");
    invalid("func (a Point, b Point) Move() {}");
    r = valid("f := func(x int) int { return x + 1; };", 1);
    assert(((struct stmt_var*)r.statements[0].value)->initValues[0].type == EXPR_TYPE_FUNCTION);
    e = expression("(func(x int) int { return x; })(1);"); assert(e.type == EXPR_TYPE_CALL);
    invalid("func f() { for (;;) { x := func() { break; }; } }");
    invalid("}"); invalid("("); invalid("x ="); invalid("f("); invalid("a[");
    invalid("func f() { if (x) "); invalid("for ("); invalid("switch { case 1:");
    invalid("struct X {"); invalid("interface X {"); invalid("import (");
    valid("", 0);
    char *deep = malloc(4 * PARSER_MAX_RECURSION_DEPTH + 4);
    size_t n = 2 * PARSER_MAX_RECURSION_DEPTH;
    memset(deep, '(', n); deep[n] = 'x'; memset(deep + n + 1, ')', n);
    deep[2*n+1] = ';'; deep[2*n+2] = 0;
    invalid(deep); free(deep);
    tokenlist empty = {0}; assert(parseTokens(&empty).hasError); checks++;
    tokenlist *missingEOF = scanTokens("x;", "missing-eof"); missingEOF->top--;
    assert(parseTokens(missingEOF).hasError); free(missingEOF); checks++;
    r = valid("items := {5, 4, 3, 2, 1};", 1);
    assert(((struct stmt_var*)r.statements[0].value)->initValues[0].type == EXPR_TYPE_COMPOSITE);
    e = expression("Point{x: 1, y: 2,};"); assert(((struct expr_composite*)e.underlying)->nEntries == 2);
    valid("nested := {{1, 2}, {3, 4}};", 1);
    valid("import (\n\"io\"\n\"math\"\n);", 2);
    invalid("x := {1 2};"); invalid("x := {a: 1, 2};");
    invalid("switch { case 1: if (x) fallthrough; case 2: break; }");
    truncatedInputs("package main; import (\"io\", \"math\" as m); func f(x int) { for (i := 0; i < x; i++) { if (i == 2) continue; } return; }");
    truncatedInputs("struct Point { x, y int; } enum E { A, B = 2 } interface I { M(x int) int; } type X = []Point;");
    truncatedInputs("func (self *Point) F(x int) int { switch (x) { case 1: return 1; default: return 2; } }");
    truncatedInputs("f := func[T any](x T) T { return x; }; x := Point{x: 1, y: 2};");
    e = expression("\"hello\";");
    assert(!strcmp(((struct expr_literal*)e.underlying)->value, "hello"));
    e = expression("`first\nsecond`;");
    assert(!strcmp(((struct expr_literal*)e.underlying)->value, "first\nsecond"));
    valid("func f() { for (i, value := range items) { use(i, value); } }", 1);
    valid("func f() { for (index, value := range items) {} }", 1);
    valid("func f() { for (i, index := range items) {} }", 1);
    valid("index;", 1);
    valid("use(index);", 1);
    tokenlist *indexTokens = scanTokens("index", "index-identifier");
    assert(indexTokens->arr[0].type == IDENTIFIER); free_tokenlist(indexTokens); free(indexTokens); checks++;
    valid("index := 0; index : int = 1;", 2);
    valid("func f(index int) (int indexResult) { return index; }", 1);
    valid("func index() {}", 1);
    valid("struct Values { index, other int; }", 1);
    r = valid("struct Values { index data []int; }", 1);
    point = r.statements[0].value;
    assert(point->nFields == 1 && !strcmp(point->indexField.lexeme, "data"));
    r = valid("struct Matrix { index underlying T[height][width]; }", 1);
    point = r.statements[0].value;
    assert(point->fieldTypes[0].nTokens == 7 && !strcmp(point->indexField.lexeme, "underlying"));
    r = valid("struct Values { data []int; index data; }", 1);
    assert(!strcmp(((struct stmt_struct*)r.statements[0].value)->indexField.lexeme, "data"));
    valid("struct Values { index data; data []int; }", 1);
    valid("struct Values { index []int; index index; }", 1);
    valid("struct Values { index index []int; }", 1);
    valid("func f(values [index]int) {}", 1);
    valid("struct Values { data int[index]; }", 1);
    invalid("value := index{};"); invalid("value := pkg.index{};");
    invalid("x : index;"); invalid("x : *index;"); invalid("x : []index;");
    invalid("func f(value index) {}"); invalid("func f() index {}");
    invalid("func f[T index](value T) {}"); invalid("func f[index any]() {}");
    invalid("type index int;"); invalid("type Alias index;");
    invalid("struct index {}"); invalid("interface index {}"); invalid("enum index { A }");
    invalid("func f(value pkg.index) {}");
    invalid("struct Values { index missing; }");
    invalid("struct Values { index a, b []int; }");
    invalid("struct Values { index a []int; index a; }");
    invalid("struct Values { index; }");
    invalid("struct Values { index data index; }");
    // Type bodies and brackets must contain grammar, not just balanced tokens.
    valid("type Record struct { x, y int; names []string; };", 1);
    valid("type Indexed struct { index data []int; };", 1);
    valid("type Indexed struct { index data; data []int; };", 1);
    valid("type Reader interface { Read(buffer []byte) (int, error); Close(); };", 1);
    valid("type Constraint interface { ~int | ~string; Reader; };", 1);
    valid("func nested[T interface { M(x int) int; ~int | ~string; }](x T) {}", 1);
    valid("type Nested Box[map[string][]int, struct { x int; }, func()];", 1);
    valid("type Array [2 * (pkg.Width + 1)]int;", 1);
    valid("type Matrix int[height + 1][width << 2];", 1);
    valid("type DynamicArray []int; type Suffix int[];", 2);
    valid("type Callback func();", 1);
    invalid("type Bad struct { garbage; };");
    invalid("type Bad struct { y int x int; };");
    invalid("type Bad struct { x int; x string; };");
    invalid("type Bad struct { index missing; };");
    invalid("type Bad struct { index a, b []int; };");
    invalid("type Bad interface { M(x int y int); };");
    invalid("type Bad interface { M(); M(); };");
    invalid("type Bad interface { func int; };");
    invalid("type Bad [1.foo]int;");
    invalid("type Bad interface { ~; };");
    invalid("type Bad interface { int |; };");
    invalid("type Bad interface { M() int garbage; };");
    invalid("type Bad Box[int string];");
    invalid("type Bad Box[,int];");
    invalid("type Bad Box[int,,string];");
    invalid("type Bad Box[map[string]];");
    invalid("type Bad Box[struct { int; }];");
    invalid("type Bad [1 +]int;");
    invalid("type Bad [1 2]int;");
    invalid("type Bad [a = 2]int;");
    invalid("type Bad [1, 2]int;");
    invalid("type Bad int[width +];");
    invalid("type Bad int[(width];");
    truncatedInputs("type Record struct { values Box[int, string]; }; type C interface { M(x []int) int; ~int | ~string; };");
    e = expression("number->int8;");
    assert(e.type == EXPR_TYPE_TYPECAST);
    struct expr_typecast *cast = e.underlying;
    assert(cast->type.nTokens == 1 && !strcmp(cast->type.tokens[0].lexeme, "int8"));
    assert(cast->child.type == EXPR_TYPE_IDENTIFIER);
    e = expression("ptr->(string*);");
    assert(e.type == EXPR_TYPE_TYPECAST);
    cast = e.underlying;
    assert(cast->type.nTokens == 2 && cast->type.tokens[1].type == STAR);
    e = expression("funcPtr->(string()*);");
    assert(e.type == EXPR_TYPE_TYPECAST);
    cast = e.underlying;
    assert(cast->type.nTokens == 4 && cast->type.tokens[3].type == STAR);
    e = expression("funcPtr->((int, float)(int)*);");
    assert(e.type == EXPR_TYPE_TYPECAST);
    cast = e.underlying;
    assert(cast->type.nTokens == 9 && cast->type.tokens[8].type == STAR);
    e = expression("number->int8 * factor + 2;");
    assert(bin(bin(e, EXPR_ADD)->left, EXPR_MULTIPLY)->left.type == EXPR_TYPE_TYPECAST);
    e = expression("(a + b)->int;");
    assert(e.type == EXPR_TYPE_TYPECAST && ((struct expr_typecast*)e.underlying)->child.type == EXPR_TYPE_BINARY);
    e = expression("ptr->(string**).field;");
    assert(e.type == EXPR_TYPE_DOT && ((struct expr_dot*)e.underlying)->parent.type == EXPR_TYPE_TYPECAST);
    e = expression("funcPtr->(string()*)();");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_TYPECAST);
    e = expression("number->int->int8;");
    assert(e.type == EXPR_TYPE_TYPECAST && ((struct expr_typecast*)e.underlying)->child.type == EXPR_TYPE_TYPECAST);
    valid("value := number->UnknownType;", 1);
    valid("value := ptr->(pkg.Type*);", 1);
    valid("value := ptr->([]int);", 1);
    valid("value := ptr->(Box[int]);", 1);
    valid("value := funcPtr->((int, float)(string*, pkg.Type)*);", 1);
    valid("type Callback (int, float)(string*)*;", 1);
    valid("func f(ptr string*, callback int(int)*) {}", 1);
    e = expression("int8(number);"); assert(e.type == EXPR_TYPE_CALL);
    e = expression("string*(ptr);"); bin(e, EXPR_MULTIPLY);
    e = expression("f(x)->(int);"); assert(e.type == EXPR_TYPE_TYPECAST);
    e = expression("array[i]->int;"); assert(e.type == EXPR_TYPE_TYPECAST);
    e = expression("a - b > c;"); bin(bin(e, EXPR_GREATER_THAN)->left, EXPR_SUBTRACT);
    invalid("ptr->string*;"); invalid("ptr->pkg.Type;");
    invalid("ptr->[]int;"); assert(expression("ptr->Box[int];").type == EXPR_TYPE_TYPECAST);
    invalid("funcPtr->string()*;"); invalid("funcPtr->(int, float)(int)*;");
    invalid("number->;"); invalid("number->();"); invalid("number->(int;");
    invalid("number->(int garbage);"); invalid("number->index;");
    invalid("funcPtr->((int, float)(int x int y)*);");
    truncatedInputs("x := funcPtr->((int, float)(int)*); y := ptr->(string*); z := number->int8;");
    e = expression("identity[int](x);");
    assert(e.type == EXPR_TYPE_CALL);
    struct expr_call *genericCall = e.underlying;
    assert(genericCall->func.type == EXPR_TYPE_INSTANTIATION && genericCall->nParams == 1);
    struct expr_instantiation *instance = genericCall->func.underlying;
    assert(instance->parent.type == EXPR_TYPE_IDENTIFIER && instance->nTypeArguments == 1);
    assert(instance->typeArguments[0].nTokens == 1 &&
           !strcmp(instance->typeArguments[0].tokens[0].lexeme, "int"));
    e = expression("pkg.make[pkg.Item, []int, Box[string],](x, y);");
    genericCall = e.underlying;
    assert(e.type == EXPR_TYPE_CALL && genericCall->nParams == 2);
    assert(genericCall->func.type == EXPR_TYPE_INSTANTIATION);
    instance = genericCall->func.underlying;
    assert(instance->parent.type == EXPR_TYPE_DOT && instance->nTypeArguments == 3);
    assert(instance->typeArguments[0].nTokens == 3 &&
           instance->typeArguments[0].tokens[1].type == DOT);
    assert(instance->typeArguments[1].nTokens == 3 &&
           instance->typeArguments[1].tokens[0].type == LEFT_BRACKET);
    assert(instance->typeArguments[2].nTokens == 4 &&
           instance->typeArguments[2].tokens[3].type == RIGHT_BRACKET);
    valid("apply[map[string][]int, func(x int) (int), *pkg.Value](value);", 1);
    valid("apply[interface { ~int | ~string; }, struct { x int; }](value);", 1);
    valid("func run() { result := identity[int](1); defer cleanup[int](); "
          "call: identity[int](1); }", 1);
    e = expression("factory[int]()[0].field;");
    assert(e.type == EXPR_TYPE_DOT);
    struct expr_dot *genericDot = e.underlying;
    assert(genericDot->parent.type == EXPR_TYPE_INDEX);
    struct expr_index *genericIndex = genericDot->parent.underlying;
    assert(genericIndex->parent.type == EXPR_TYPE_CALL &&
           ((struct expr_call*)genericIndex->parent.underlying)->func.type == EXPR_TYPE_INSTANTIATION);
    e = expression("factory[int]()[string](x);");
    assert(e.type == EXPR_TYPE_CALL &&
           ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INSTANTIATION);
    e = expression("a[i];"); assert(e.type == EXPR_TYPE_INSTANTIATION);
    e = expression("a[i + 1](x);");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INDEX);
    e = expression("a[0](x);");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INDEX);
    e = expression("(a[i])(x);");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INSTANTIATION);
    e = expression("a[i](x);");
    assert(e.type == EXPR_TYPE_CALL &&
           ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INSTANTIATION);
    e = expression("f[](x);");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INDEX);
    invalid("f[int,,string](x);");
    invalid("f[[]](x);"); invalid("f[int string](x);");
    invalid("f[int](;"); invalid("f[int](x;");
    invalid("f[map[]int](x);");
    e = expression("f[map[string]](x);");
    assert(e.type == EXPR_TYPE_CALL && ((struct expr_call*)e.underlying)->func.type == EXPR_TYPE_INDEX);
    truncatedInputs("result := pkg.pair[Box[int], []string,](x, y).field[0];");
    r = valid("interface Scalar[T any] { -Matrix; =Required[T]; +Optional; "
              "self * T; T + self; self == pkg.Value; self += T; Read() T; }", 1);
    iface = r.statements[0].value;
    assert(iface->nConstraints == 7 && iface->nMethods == 1);
    assert(iface->constraints[0].nTokens == 2 && iface->constraints[0].tokens[0].type == MINUS);
    assert(iface->constraints[1].nTokens == 5 && iface->constraints[1].tokens[0].type == EQUALS);
    assert(iface->constraints[2].tokens[0].type == PLUS);
    assert(iface->constraints[3].nTokens == 3 && iface->constraints[3].tokens[1].type == STAR &&
           !strcmp(iface->constraints[3].tokens[0].lexeme, "self"));
    assert(iface->constraints[4].tokens[1].type == PLUS &&
           !strcmp(iface->constraints[4].tokens[2].lexeme, "self"));
    valid("type Scalar interface { -Matrix; =Required; +Optional; self * T; T + self; };", 1);
    valid("func custom[T interface { -Matrix; self * int; }](value T) {}", 1);
    valid("interface Complex { self * map[string][]int; "
          "Box[int]* + self; self << int; self && bool; "
          "self * interface { M() int; }; }", 1);
    valid("interface Legacy { *Pointer; Value*; ~int | ~string; Reader; }", 1);
    invalid("interface Bad { -; }"); invalid("interface Bad { =; }");
    invalid("interface Bad { +; }"); invalid("interface Bad { --Matrix; }");
    invalid("interface Bad { -index; }"); invalid("interface Bad { self *; }");
    invalid("interface Bad { * self; extra }");
    invalid("interface Bad { self + T + U; }");
    invalid("interface Bad { self * index; }");
    invalid("interface Bad { self * T }");
    invalid("interface Bad { self -> T; }");
    invalid("type Bad interface { self *; };");
    r = parse("interface Bad { self *; } interface Good { -Matrix; self * int; }");
    assert(r.hasError && r.nStatements == 1 && r.statements[0].type == STMT_INTERFACE &&
           ((struct stmt_interface*)r.statements[0].value)->nConstraints == 2);
    truncatedInputs("interface Scalar[T any] { -Matrix; =Base[T]; +Other; self * T; T + self; }");
    fixture("testcode/helloWorld.oc", 2);
    fixture("testcode/loops.oc", 2);
    printf("%zu parser feature checks passed\n", checks);
}
