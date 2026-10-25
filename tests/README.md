# Function parser tests

Run from the repository root:

```sh
clang -std=c2x -fsanitize=address,undefined -g -I. \
  tests/function_parser_test.c scanner.c token.c dynarray.c -lm \
  -o /tmp/ocean-function-parser-test
ASAN_OPTIONS=detect_leaks=0 /tmp/ocean-function-parser-test
```

The suite checks AST contents and rejection of malformed input. Leak detection
is disabled because the compiler does not yet have complete AST/token cleanup.
No allocator zero-fill workaround is used.

Supported declaration examples:

```Ocean
func main() {}
func add(int a, b) int { return a + b; }
func split(string text) (string head, tail) { return; }
func log(string prefix, ...string values) {}
func identity[T any](T value) T { return value; }
func pair[T, U any](T first, U second) (T, U) { return first, second; }
func constrained[T ~int | ~string](T value) T { return value; }
```

Value parameters and named results put types first; grouped names share a type.
Generic type parameters retain Go's name-before-constraint syntax. Unnamed
parameters/results and trailing commas are accepted. Variadics must be last.
Existing Ocean statement syntax still uses explicit semicolons; `return` also
allows an immediately following closing brace.

The AST stores type/constraint tokens for subsequent type resolution. Inline struct/interface bodies, generic argument lists, and array dimensions
are grammar-validated before their token spans are retained. Struct fields use
type-first declarations; interfaces accept method signatures, embedded types,
and union constraints. Dimensions accept integer literals, qualified names,
parentheses, unary operators, and binary operators. Ambiguous suffixes such as
`T[N]` remain unresolved until type checking. The parser does not evaluate array
lengths, perform semantic type checking, verify return arity/types, or instantiate
generics. The function-type spelling `func() int` is an unnamed function type
returning `int`. Use `func() () callback` for a named parameter whose function
type has no results, or `func() int callback` for a named callback returning `int`.

These tests include parser.c to exercise malformed declarations directly as well
as through parseTokens; do not compile parser.c separately into the test binary.

## Parser feature suite

All suites are registered with CTest:

```sh
cmake -S . -B /tmp/ocean-parser-build -DCMAKE_C_COMPILER=clang -DBUILD_TESTING=ON
cmake --build /tmp/ocean-parser-build
ctest --test-dir /tmp/ocean-parser-build --output-on-failure
```

Use a fresh build directory if it was previously configured with inconsistent
compiler settings. The feature suite runs from the repository root and includes
the existing hello-world and loop examples.

For AddressSanitizer/UndefinedBehaviorSanitizer coverage:

```sh
clang -std=c2x -fsanitize=address,undefined -g -I. \
  tests/parser_features_test.c scanner.c token.c dynarray.c -lm \
  -o /tmp/ocean-parser-features-test
ASAN_OPTIONS=detect_leaks=0 /tmp/ocean-parser-features-test
```

The feature suite covers expression associativity/precedence, packages/imports,
type/struct/enum/interface declarations, extern signatures, branches, labels,
switches, deferred calls, method receivers, anonymous functions, composite
literals, nesting limits, and recovery. It also tries every token prefix of four
representative programs, checking termination and bounds safety. The suites also cover valid and malformed type bodies, generic arguments, array
dimensions, and truncated type declarations.

Additional syntax examples:

```Ocean
package main;
import "math" as m;
import ("io", _ "side-effects");
type Count int;
type Alias = []int;
struct Point { int x, y; }
enum Color { RED, GREEN = 2, BLUE, }
interface Reader { Read([]byte buffer) (int, error); }
extern func write([]byte bytes) int;
func (*Point self) Move(int x) int { return x; }
f := func(int x) int { return x + 1; };
points := {Point{x: 1, y: 2}, Point{x: 3, y: 4}};
func run(int x) {
    defer cleanup();
    outer: for (;;) { if (x == 0) break outer; continue; }
    switch (x) {
        case 1, 2: x++; fallthrough;
        case 3: break;
        default: return;
    }
}
```

Imports are AST declarations, not package loading. Type names, constraints,
label targets, return types, and extern linkage still require semantic analysis.
See the [pending feature inventory](pending/README.md) for remaining limitations.

## Switch extensions and anonymous generics

Switch headers accept a declaration or expression initializer followed by an
explicit semicolon. Type switches retain the guard, optional binding, and
complete case type spans in the AST:

```Ocean
switch (x := next(); x) { case 1: break; default: break; }
switch (prepare(); item := value.(type)) {
    case int, []string: use(item);
    default: break;
}
identity := func[T any](T value) T { return value; };
```

Type assertions use `value.(Type)`. The special `.(type)` guard is restricted
to the switch header, optionally with one inferred binding. Type-switch cases
contain types and disallow fallthrough. An initializer may also precede a
condition-only switch, as in `switch (prepare();) { ... }`. Binding resolution,
assertion validity, and type-case compatibility remain semantic analysis.
Ocean requires explicit statement separators and does not insert semicolons.

## Struct indexing and the name `index`

`index` is scanned as an identifier. It may name variables, parameters, fields,
functions, or range bindings, but cannot name a type or type parameter. In a
struct body it introduces indexing delegation in either form:

```Ocean
struct Values { index []int data; }
struct Matrix { T[height][width] underlying; index underlying; }
```

The AST's `stmt_struct.indexField` identifies the delegated field; EOF_TOKEN
means no field was designated. A separate directive can precede the field
it names. The parser validates uniqueness and field existence; the type checker
must validate that the target can be indexed. The expression `self[]` accesses the delegated collection, while `self[i]`
accesses an element. Both bracketed and angle-bracket parameters are accepted.

## Casts

Use `expression->Type` for a single-token type. Parentheses are required around
multi-token targets:

```Ocean
number->int8;
ptr->(string*);
funcPtr->(string()*);
funcPtr->((int, float)(int)*);
ptr->(pkg.Type*);
```

The outer parentheses delimit the target and are excluded from its AST token
span. Pointer stars follow the type; function types put results before their
parameter list. These type forms are also accepted in declarations. Casts bind
with calls, indexing, and member access, and chain left to right. Use
`(a + b)->int` to cast an entire arithmetic expression. `number->int * scale`
multiplies the converted number by `scale`.

`int8(number)` is an ordinary call. Arrow casts accept unresolved type names;
name resolution and checking whether a conversion is legal remain semantic work.
For a qualified target, write `value->(pkg.Type)`. To access a member of a cast to
a single-token target, write `(value->Type).field` or `value->(Type).field`.

## Labels

Labels may precede any statement and may be chained:

```Ocean
func run() {
    done: return;
    update: count++;
    first: second: goto done;
    declaration: value := 1;
}
```

Colon declarations take precedence when a complete type is followed by `;` or
`=`. Thus `value: int;` remains a declaration. A zero-argument call such as
`label: work();` also looks like a function-type declaration; write
`label: { work(); }` to label that call. Labels preserve the body's context
restrictions: for example, a labeled break still requires a loop or switch,
and fallthrough must still be a direct switch-clause statement. Label target
resolution remains semantic work.

## Named interface constraints

Named interfaces accept the same embedded types and union terms as inline
interfaces, mixed with method signatures:

```Ocean
interface Scalar {
    ~int | ~string;
    pkg.Base;
    Box[int];
    Format() string;
}
```

Each semicolon-terminated embedded type or union is retained in
`stmt_interface.constraints` as one token span; the semicolon is excluded.
Methods remain in `stmt_interface.methods`. The parser validates the grammar;
type resolution, type-set compatibility, and constraint satisfaction belong to
semantic analysis. Angle-bracket interface parameters such as
`interface Scalar<T: any> { ... }` are also accepted.

## Generic calls

A nonempty, grammar-valid bracketed type list is parsed as generic
instantiation, including standalone generic function values:

```Ocean
identity[int](value);
pkg.pair[pkg.Item, []string,](first, second);
apply[map[string]int, func(int x) (int)](callback);
callback := identity[int];
identity<int>(value);
```

The call's `func` is an `EXPR_TYPE_INSTANTIATION` node holding the original
callee and one `typeTokens` span per argument. Argument spans exclude list
separators and outer brackets; nested type syntax is preserved. A trailing
comma is accepted. Angle arguments are accepted before calls, composite
literals, member access, and expression delimiters. Comparisons such as
`a < b > c` and shift expressions keep their binary AST forms.

`a[i]` and `a[i](x)` are grammatically ambiguous: they receive instantiation
nodes, and name resolution must decide whether `i` denotes a type or an index
value. Parentheses around `a[i]` preserve this ambiguity. Expressions that do
not form type arguments, such as `a[0]` or `a[i + 1]`, remain indexing. Generic
instantiation, index resolution, and type checking still belong to semantic analysis.

## Generic structs and interfaces

Structs and named interfaces use the same bracketed, name-before-constraint
parameter grammar as generic functions:

```Ocean
struct Pair[T, U any] {
    T first;
    U second;
}

interface Mapper[T any, U ~int | ~string] {
    Base[T];
    Map(T value) U;
}
```

Grouped names share a constraint; trailing commas are accepted. Empty lists,
missing constraints, duplicate parameter names, and reserved type names are
rejected. Both AST declaration nodes retain `nTypeParams`, `typeParamNames`,
and `typeParamConstraints`; each constraint is a complete token span, excluding
parameter separators. Struct constraints now use `typeTokens` rather than the
old unused single-token `typeParamInterfaces` field.

Fields, indexing delegation, embedded interfaces, and method signatures may
reference parameters. Binding those references and enforcing constraints remain
semantic work. Angle parameters use a colon before each constraint group,
for example `struct Pair<T, U: any> { T first; U second; }`. Nested type
arguments such as `Box<Box<int>>` are accepted without changing shift tokens.

## Struct value parameters

An optional value-parameter list follows the struct name and any type parameters:

```Ocean
struct Matrix[T any](u64 width, height) {
    index T[height][width] underlying;
}

struct Buffer(u64 capacity) {
    byte[capacity] data;
}
```

Value parameters use type-first declarations. Grouped names share a type;
trailing commas and empty lists are accepted. Each parameter must have a name.
Duplicate names, collisions with type parameters, blank/unnamed parameters,
variadics, and default values are rejected. `index` remains a valid value name.

The AST retains `nVarParams`, `varParamNames` (tokens), and `varParamTypes`
(complete type spans). These replace the previously unused string-pointer
arrays. Parameter references in field dimensions remain tokens for subsequent
name resolution and constant evaluation. This adds declaration parsing; value
argument application and struct instantiation are not implemented here.

## Operator methods and custom interface constraints

Struct bodies accept operator methods with an implicit `self` receiver:

```Ocean
struct Number[T any] {
    T value;
    +(T other) T { return self.value + other; }
    -() T { return -self.value; }
}

interface Scalar[T any] {
    -Matrix;
    =Required;
    +Optional;
    self * T;
    T + self;
}
```

Membership prefixes retain the meanings proposed in `structEtc.oc`: `-Type`
excludes a type, `=Type` requires it, and `+Type` permits it. Infix requirements
must have `self` as at least one complete operand; the other operand uses the
type grammar. Named and inline interfaces share this syntax. Complete terms
remain token spans in `stmt_interface.constraints` (or the enclosing inline
type span), so semantic analysis can interpret their meaning later. Existing
embedded types and union constraints take precedence over infix requirements.
`self *;` is rejected as an incomplete multiplication requirement.

Operator methods are stored in `stmt_struct.operatorMethods` with
`nOperatorMethods`; each uses `stmt_func`, with the symbol token in `name` and
`self` in `receiverName`. The enclosing struct supplies the receiver type;
`receiverType` is empty. Parameters and results cannot redeclare `self`.
Signatures use the existing function parameter/result grammar and require a
body. Overloads can repeat an operator with different signatures; duplicate
signature detection, operator arity/result rules, constraint satisfaction, and
overload selection remain semantic work.

Supported symbols are arithmetic, bitwise, comparison, shift, and logical
operators, `!`, `~`, `++`, `--`, and supported compound assignments. Plain `=`,
`~=`, indexing/call operators, and operator-specific generic parameter lists
are not accepted as operator method names. Pointer/function fields keep their
existing grammar. Angle-bracket parameters and empty-index expressions
(`self[]`) are supported.

## Expanded coverage suites

The CTest suite now contains:

- `function_parser_test`: function signatures and malformed declarations.
- `parser_features_test`: language features, AST structure, recovery, and truncation.
- `parser_extensions_test`: angle/anonymous generics, standalone instantiation,
  delegated collection access, switch initializers/type switches, function types,
  explicit semicolons, malformed declarations, recovery, and token prefixes.
- `parser_edges_test`: unary/compound operators, float AST values, ternary
  associativity, if initializers/else branches, loop modes, branch labels,
  empty composites/switch cases, and invalid input/nesting limits.
- `scanner_test`: every keyword and operator token, literal values and lexemes,
  source positions, nested comments, escape sequences, integer boundaries,
  character literals, Unicode placeholder warnings, and rejected input.
- `dynarray_test`: all five generated list types, growth, shrinking, insertion
  order, independent snapshots, emptying/reuse, and invalid operations.
- `token_test`: all real token names, unknown-token fallback, and printer output.
- `ast_test`: supported literal, unary, binary, ternary, and call debug output,
  including identifiers, empty/multi-argument calls, and long strings.
- `hashmap_test`: the vendored dependency's enabled public API, including
  replacement callbacks, get-or-insert behavior, resizing through every insertion
  API, iteration order/early exit, clearing/reuse, and binary/zero-length keys.
  The optional removal feature is disabled in the dependency and is not tested.
- `cli_test`: missing arguments/files, valid and empty files, multiple files,
  read failures, scanning errors, and parsing errors, including exit statuses.

Scanner, array, token, and AST tests use POSIX subprocesses to check APIs that
exit or abort, so those targets are registered on Unix systems. Assertions stay
enabled in release builds. Test subprocesses disable core dumps; each CTest
entry has a timeout. The debug AST printer remains incomplete for newer AST
node kinds; its supported forms and unknown-node rejection are tested.

Run the complete suite under Clang sanitizers:

```sh
cmake -S . -B /tmp/ocean-sanitizers -DCMAKE_C_COMPILER=clang \
  -DBUILD_TESTING=ON '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -g'
cmake --build /tmp/ocean-sanitizers -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/ocean-sanitizers --output-on-failure
```

Leak detection remains disabled because full parser/token destruction is not
implemented. Address and undefined-behavior checks remain active, including
inside subprocesses.

With Clang, `llvm-profdata`, and `llvm-cov` installed, generate line/branch
coverage and an HTML report:

```sh
tests/run_coverage.sh /tmp/ocean-coverage-build
```

The optional directory defaults to a fresh temporary directory. Each run uses
a fresh profile directory. Reports exclude tests and the vendored `c-hashmap`
dependency. Coverage runs exclude pending acceptance cases so unsupported
features do not inflate regression coverage. The expanded suite measured **94.37% first-party line coverage**
and **85.04% branch coverage**. Parser line coverage is 95.04%, scanner 93.93%,
and token printing 100%. These are coverage measurements, not claims that all
possible inputs are tested. Allocation failures and some defensive paths remain
uncovered; aborting children do not flush LLVM profiles, so their tested error
paths can still appear uncovered in the report.

Regression tests exposed and prompted fixes for double-decrementing array
pops, shrinking to zero capacity, insertion shifts, macro argument grouping,
incorrect floating-point values, malformed/base-prefixed numbers, escape
conversion, character literal bounds/quoting, accepted unterminated comments,
ignored invalid characters, missing/incorrect token names, borrowed identifier
ownership and fixed-size buffers in debug printing, empty/multi-argument call
printing, and incorrect command-line success statuses on errors.

## Pending incomplete features

The [pending inventory](pending/README.md) defines 64 future acceptance
contracts across 21 incomplete feature areas. CTest lists them individually
under the `pending` label. Unmet contracts are reported as skipped/XFAIL;
unexpected successes fail so they can be promoted to regression tests.
Twelve cases probe current APIs; 52 semantic/cleanup fixtures explicitly
await compiler-stage adapters. Proposed syntax is marked in the inventory.

```sh
ctest --test-dir /tmp/ocean-parser-build -L pending --output-on-failure
ctest --test-dir /tmp/ocean-parser-build -LE pending --output-on-failure
```

The default test run includes both groups. Python 3 is now required when
`BUILD_TESTING` is enabled. Inventory validation and 26 harness self-tests are
regular passing tests. Crashes and sanitizer reports fail the pending suite;
they are not treated as missing features.
