# Function parser tests

CLI tests invoke `ocean build`, `ocean build --check`, and
`ocean build --emit-llvm`; source fixtures include `package main;`. IR assertions
use package-qualified symbols and distinguish source function bodies from runtime
helpers. Default executable names follow the package directory name.

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
func add(a, b int) int { return a + b; }
func split(text string) (string head, tail) { return; }
func log(prefix string, values ...string) {}
func identity[T any](value T) T { return value; }
func pair[T, U any](first T, second U) (T, U) { return first, second; }
func constrained[T ~int | ~string](value T) T { return value; }
```

Value parameters put names before types (`a, b int`); grouped names share a type.
Named results retain type-first syntax. Method receivers use name-before-type syntax.
Extern declarations use C ordering: `extern int write(i32 fd, buffer<i8>[] data, int count);` or
`extern void done(void);`. Parameter names are optional; `()` and `(void)`
both denote no parameters. A trailing `...` is accepted after fixed parameters.
Generic type parameters retain Go's name-before-constraint syntax. Unnamed
parameters/results and trailing commas are accepted. Variadics must be last.
Existing Ocean statement syntax still uses explicit semicolons; `return` also
allows an immediately following closing brace.

The AST stores type/constraint tokens for subsequent type resolution. Inline struct/interface bodies, generic argument lists, and array dimensions
are grammar-validated before their token spans are retained. Struct fields use
name-before-type declarations (`x, y int`); interfaces accept method signatures, embedded types,
and union constraints. Dimensions accept integer literals, qualified names,
parentheses, unary operators, and binary operators. Ambiguous suffixes such as
`T[N]` remain unresolved until type checking. The parser does not evaluate array
lengths, perform semantic type checking, verify return arity/types, or instantiate
generics. The function-type spelling `func() int` is an unnamed function type
returning `int`. Use `callback func() ()` for a named parameter whose function
type has no results, or `callback func() int` for a named callback returning `int`.

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
struct Point { x, y int; }
enum Color { Red, Green := 2, Blue(int), }
interface Reader { Read(buffer []byte) (int, error); }
extern int write([]byte bytes);
func (self *Point) Move(x int) int { return x; }
f := func(x int) int { return x + 1; };
points := {Point{x: 1, y: 2}, Point{x: 3, y: 4}};
func run(x int) {
    defer cleanup();
    outer: for (;;) { if (x == 0) break outer; continue; }
    switch (x) {
        case 1, 2: x++; fallthrough;
        case 3: break;
        default: return;
    }
}
```

These parser-only tests inspect import AST declarations. Package loading is
covered separately by `package_test.py`. Type names, constraints,
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
identity := func[T any](value T) T { return value; };
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
struct Values { index data []int; }
struct Matrix { underlying T[height][width]; index underlying; }
```

The AST's `stmt_struct.indexField` identifies the delegated field; EOF_TOKEN
means no field was designated. A separate directive can precede the field
it names. The two-identifier form `index data;` is reserved for a delegation
directive. The parser validates uniqueness and field existence; the type checker
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
`interface Scalar<T any> { ... }` are also accepted.

## Generic calls

A nonempty, grammar-valid bracketed type list is parsed as generic
instantiation, including standalone generic function values:

```Ocean
identity[int](value);
pkg.pair[pkg.Item, []string,](first, second);
apply[map[string]int, func(x int) (int)](callback);
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
    first T;
    second U;
}

interface Mapper[T any, U ~int | ~string] {
    Base[T];
    Map(T value) U;
}
```

Grouped names share a constraint; trailing commas are accepted. Omitted
constraints default to `any`: `<T>` is equivalent to `<T any>`, and `<T, U>`
defaults both names to `any`. This also applies to square-bracket declarations,
interfaces, and anonymous functions. Empty lists, duplicate parameter names, and reserved type names are
rejected. Both AST declaration nodes retain `nTypeParams`, `typeParamNames`,
and `typeParamConstraints`; each constraint is a complete token span, excluding
parameter separators. Struct constraints now use `typeTokens` rather than the
old unused single-token `typeParamInterfaces` field.

Fields, indexing delegation, embedded interfaces, and method signatures may
reference parameters. Binding those references and enforcing constraints remain
semantic work. Angle parameters place each constraint directly after its parameter names,
for example `struct Pair<T, U any> { first T; second U; }`. Nested type
arguments such as `Box<Box<int>>` are accepted without changing shift tokens.

## Struct value parameters

An optional value-parameter list follows the struct name and any type parameters:

```Ocean
struct Matrix[T any](u64 width, height) {
    index underlying T[height][width];
}

struct Buffer(u64 capacity) {
    data byte[capacity];
}
```

Struct value parameters accept name-before-type declarations (`size int`, `height, width int`).
Legacy type-before-name declarations (`int size`) remain accepted. Grouped names share a type;
trailing commas and empty lists are accepted. Each parameter must have a name.
Duplicate names, collisions with type parameters, blank/unnamed parameters,
variadics, and default values are rejected. `index` remains a valid value name.

The complete `fieldNames`/`fieldTypes` arrays start with value parameters, in
parameter declaration order, followed by body fields. `nFields` includes this
prefix, and `nVarParams` marks its immutable extent. `varParamNames` and
`varParamTypes` are non-owning views into that prefix. Field/parameter collisions
are rejected, and indexing directives may refer to a parameter field.

The semantic checker permits parameter initialization in composites and rejects
later assignment, compound assignment, increments, simultaneous assignment, and
mutable-address escapes. Ordinary fields and referenced buffer elements remain
writable. Methods, implicit `self`, pointer receivers, and nested value fields
respect the same rules, including concrete generic type arguments and nested
generic field types. Built-in arrays have an immutable `size` parameter and
mutable `data` storage supporting copy assignments and element writes.
Value argument application remains unimplemented. Native struct values and
concrete generic struct layouts are supported by the backend.

## Interfaces and type lists

Interfaces contain either method signatures or one union of types. Type-list
interfaces do not require a final semicolon. Named and inline forms share the
same parser; methods have implied receivers, optional parameter names, and may
have generic type parameters. Signed membership and infix operator requirements
have been removed. Struct operator overloading remains unsupported.

`type_lists_test.py` checks numeric generic constraints, boxed arithmetic and
comparisons, strings, array concatenation, pointer equality, direct type-list
parameters, value widening, exact versus underlying
type matching, constraint forwarding, generic method interface satisfaction, and imported constraints.
It executes both ordinary and optimized native builds. Operator checking covers
every alternative in a generic body before specialization. Existing parser suites
cover both interface forms and AST signatures. No rejection tests were added.

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
  character literals, Unicode identifiers/scalars/escapes, scientific exponents, unsigned magnitude boundaries, and rejected input.
- `dynarray_test`: all five generated list types, growth, shrinking, insertion
  order, independent snapshots, emptying/reuse, and invalid operations.
- `token_test`: all real token names, unknown-token fallback, and printer output.
- `ast_test`: supported literal, unary, binary, ternary, and call debug output,
  including identifiers, empty/multi-argument calls, and long strings.
- `hashmap_test`: the vendored dependency's enabled public API, including
  replacement callbacks, get-or-insert behavior, resizing through every insertion
  API, iteration order/early exit, clearing/reuse, and binary/zero-length keys.
  The optional removal feature is disabled in the dependency and is not tested.

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

The [pending inventory](pending/README.md) defines 48 future acceptance
contracts across 16 incomplete feature areas. CTest lists them individually
under the `pending` label. Unmet contracts are reported as skipped/XFAIL;
unexpected successes fail so they can be promoted to regression tests.
Six cases probe current APIs; 42 semantic/cleanup fixtures explicitly
await compiler-stage adapters. Proposed syntax is marked in the inventory.

```sh
ctest --test-dir /tmp/ocean-parser-build -L pending --output-on-failure
ctest --test-dir /tmp/ocean-parser-build -LE pending --output-on-failure
```

The default test run includes both groups. Python 3 is now required when
`BUILD_TESTING` is enabled. Inventory validation and 26 harness self-tests are
regular passing tests. Crashes and sanitizer reports fail the pending suite;
they are not treated as missing features.

`semantic_test` exercises the standalone `analyzeSemantics` pass and its CLI
integration. The pass checks lexical names, duplicate declarations, basic
operators, initializers, assignments, call arguments, return values and struct
fields. It rewrites ambiguous single-name bracket expressions to index nodes
when the name resolves to a value, and substitutes explicit generic arguments
in function signatures. Each input file is currently analyzed independently.
Package loading, generic constraints, constant dimensions,
label validation and control-flow completeness are not implemented yet.

`backend_test.py` compiles single-file programs through both the native driver
and saved LLVM IR, runs the executables, and checks exit status and output.
It covers recursion, numeric casts, extern calls, branches, nested loops,
shadowing, named results, short-circuit evaluation, and output preservation on
failure. These tests require Clang and a working native linker/C runtime.

Native function tests cover empty and nonempty variadic argument packs, fixed
arguments, array expansion, rejection of buffer expansion, variadic forwarding,
normal array fields, `len()`, and checked variadic indexing. They also cover C variadic ABI calls, heterogeneous
multiple returns, named results, forwarding, unpacking, discards, and simultaneous
assignment. Calls producing several values are expanded only when they are the
sole expression in a declaration, return, argument list, or assignment RHS.

Runtime buffer tests cover once-only bound evaluation, arithmetic and function
call bounds, unsized parameter mutation, inferred backing storage, pointer
aliases, C pointer interoperability, and stack restoration on block exit,
`break`, and `continue`. Buffers carry no runtime length metadata.

Built-in array tests verify `T[]`/`T[count]`, runtime counts, zero-length arrays,
inline `int size` followed by payload bytes (including a native memory comparison),
`.size`/`.data` access, `len()` derived from bytes, scalar element widths, nested
arrays, arrays of fixed buffers, aliasing, function calls, stack restoration,
and malformed calls and element types. `sizeof` tests ensure operands are not
executed and that element storage sizes match native LLVM layout.

`array_bounds_test.py` verifies bounds failures for reads, writes, increments,
negative indices, empty and nested arrays, element widths, and recursion.
Each case runs both the native compiler output and independently optimized
LLVM IR, checks the exact source location and caret, and validates the call
trace. Source replacement/deletion and filenames containing percent signs and
quotes exercise embedded diagnostics. Raw buffer indexing stays unchecked.

Array data-copy tests cover buffers, overlapping/self copies, literal capacity
errors, zero/empty-string padding, nested defaults, RHS evaluation order, and
unchanged array metadata. Struct parameter tests verify prefix order, canonical
AST views, initialization, read-only mutations/addresses, and mutable payloads.

Variadic array tests verify that parameters use the same native pointer-to-inline
array representation as `T[]` (including a native byte comparison), without a
private pointer/length view. They cover explicit items plus expansion, reference
sharing versus concatenation copies, zero-item packs, ordinary array parameters,
nested element types, single evaluation, normal array data assignment, caller
stack lifetime, and bounds diagnostics in variadic callees.

Contextual literal tests cover array/buffer/struct destinations, inference in
nonempty declarations, nested fields and elements, multiple assignment, fixed
buffer padding, and explicit `T[]{...}` / `buffer<T>{...}` prefixes. Function
arguments and return values require explicit prefixes. Untyped call/return
literals, ambiguous empty inference, scalar destinations, mismatched elements,
and invalid keys are rejected. Native literal tests also verify single evaluation,
array expansion, storage widths, zero values, and stack restoration.


Strings are immutable heap-backed `i8` dynamic arrays with capacity equal to length.
Regression tests cover signed bytes, embedded zeros, empty strings, read-only
indexing, concatenation, reassignment, independent mutable byte copies, and C
calls using temporary data buffers. Element writes and in-place growth are
rejected. Literal storage has no implicit zero terminator.


Native struct regressions execute compiler output and optimized saved LLVM IR.
They cover positional/keyed/nested composites, zero and empty defaults, value
copies, arguments and returns, multiple returns, variadics, fixed buffers,
arrays, field mutations, pointer dereferencing and equality, recursive pointers,
local declarations, and immutable parameter fields. Generic layout assertions
verify concrete field types, reuse of instantiations, and absence of runtime
type argument fields. A linked C helper checks field access, alignment, padding,
and sizes across two instantiations through pointer parameters. Invalid inline
recursive values, expanding recursive specializations, runtime-sized inline
fields, and C struct arguments/returns by value produce explicit errors.

## Method receiver bindings

Method receivers use `name type`, just like ordinary parameters and struct fields:

```Ocean
struct Node<T any> { value T; next *Node<T>; }
func (n *Node<T>) set(value T) { n.value = value; }
```

`T` is declared by the receiver and inherits the corresponding struct parameter's
constraint. Receiver bindings are positional and may be renamed, as in
`func (n *Node<E>) get() E { return n.value; }`. Angle and square generic lists,
value receivers, and prefix or suffix pointers are accepted. The receiver must
name a struct, and its parameter count must match that struct. Each argument
must be a distinct fresh type parameter name, rather than a concrete or nested
type. Receiver bindings are scoped to the method signature and body.

Method-specific parameters can be declared separately (`set<U any>`), but cannot
redeclare a receiver binding. Receiver variables and ordinary parameters cannot
reuse a receiver type parameter's name. Named results still use type-first syntax.
Constraint spans are inherited from the struct; constraint satisfaction remains
part of the compiler's unfinished generic semantics.

Direct calls to named methods compile for concrete structs and generic struct
specializations. The backend emits one function per method, concrete receiver type, and method
type argument list, passing the receiver as an extra first argument. Pointer receivers borrow
addressable values; value receivers receive copies, including calls through a
pointer. Non-addressable temporaries cannot call pointer methods. Borrowing an
immutable value field for a pointer method is rejected. Recursive calls, calls
between methods, multiple results, and variadic parameters are supported.
Method-specific parameters use explicit call arguments, for example
`func (b Box<T>) take<U>(value U) U { return value; }` and
`b.take<int>(7)`. Both angle and square lists work, omitted declaration
constraints default to `any`, and the method bindings are independent of receiver
bindings. Type arguments are resolved in the caller's scope, allowing recursive
calls and calls between generic methods. Missing or incorrect type argument
counts are diagnosed; argument inference is not implemented. Bound method values
remain unsupported by code generation. Backend tests exercise method calls with
both unoptimized and optimized LLVM IR and verify specialization reuse.

## Pointer null values

`null` is scanned as a reserved keyword and parsed as `LITERAL_TYPE_NULL`.
It has no primitive or struct value. Typed pointer destinations accept it in
initializers, assignments, arguments, returns, casts, composite fields, and
conditional expressions. Equality is symmetric (`p == null`, `null == p`), and
null equals null. Other operators, primitive/struct destinations, and inferred
null-only variables or arrays are rejected.

Pointer defaults remain zero addresses, distinct from explicit null. Native null
uses a reserved module marker without changing pointer size or struct layout.
Pointer dereferences and struct field accesses check for this marker and trap;
value-receiver calls also check before copying a pointed-to struct. Structs and
primitives keep zero initialization. Tests cover null/zero distinctions, typed
null propagation, generic method results, linked lists, and invalid dereferences.

`semantic_diagnostics_test.py` checks type mismatch diagnostics in both frontend
and executable compilation modes. It verifies file, line and column, the source
line and caret, and expected/actual type names across operators, assignments,
conditions, calls, returns, literals, and resolved index expressions. Coverage
includes generic receiver parameters, pointers, arrays, fixed buffers, numeric
type spellings, function signatures, tabs, CRLF input, and paths with spaces.


`zero(Type)` is covered by parser and semantic suites, the AST printer suite,
and native/optimized LLVM backend tests. Cases include scalars, pointers versus
explicit null, empty arrays/strings, recursive generic structs, fixed buffers
containing reference fields, struct value parameters, generic receiver methods,
and explicitly instantiated generic functions. Tests also check returned empty
values, repeated loop use, `sizeof(zero(T))`, and malformed/non-type arguments.


`constant_builtins_test.py` inspects unoptimized LLVM IR to verify scalar and
aggregate `zero` values are constants and `sizeof` uses target-layout constant
expressions. It covers concrete generic functions/methods, padded records,
long aggregate constants, fixed buffers, reference types, and unevaluated
`sizeof` operands. The emitted module runs through native, `-O0`, and `-O2`
builds, with expected sizes obtained from the host ABI.


`array_values_test.py` verifies runtime-sized array value semantics at `-O0` and
`-O2`, including independent callee-stack copies, assignments, nested arrays,
structs, value/pointer receivers, explicit `*T[]` sharing, argument snapshots,
variadics, generic functions, caller-frame returns, and lexical scope lifetimes.
It inspects the indirect parameter ABI and tests direct, mutual, and generic
recursion returning arrays, nested arrays, and structs containing arrays. Recursive
returns use temporary heap transport that is freed after copying onto the caller stack.
Whole-value replacement through an explicit pointer must fit the destination's existing storage.


`panic_test.py` exercises panic statements in native and optimized IR, including
string expressions, literal percent signs, embedded NUL bytes, source excerpts
that survive source-file removal, recursive/generic/method call traces, terminating
control flow, and invalid messages or syntax.


`goto_test.py` checks forward and backward jumps, labels after terminated code,
nested-scope stack cleanup, retained array values, separate namespaces for generic
and array-returning function expansions, labeled loop control, and rejection of
undefined/duplicate labels or jumps into blocks or over declarations.


The panic tests also cover `return panic`, including caller-source attribution
through methods, generic functions, recursive and inlined array returns,
multiple-result and void functions, and the entry-function fallback.


`package_test.py` exercises shared directory scope, package entry points,
qualified generics and methods, capitalized exports, private declarations and
methods, filtered dot imports, shared globals and array lifetimes, import
aliases and namespace isolation, standard library resolution, module manifests,
local replacements, real Git downloads through a local URL rewrite, cache reuse,
cross-file diagnostics and caller-directed panics, file-local labels, invalid
package graphs, object output, and run cleanup on exit or signals. Tests use
`OCEAN_SRC`, `OCEAN_MOD_CACHE`, and `TMPDIR` to avoid installed sources or network
access. Both `ocean` and the legacy `ocean-c` executable are built.

`c_variadic_test.py` checks runtime C variadic expansion against a native C
fixture at `-O0` and `-O2`, including mixed fixed/variadic types, scalar return
ABIs, pointer mutation, empty expansions, and repeated calls. The backend suite
also covers heterogeneous `...any` forwarding, typed arrays forwarded to `...any`,
default promotions, and integer/floating-point register overflow. libffi development
headers are required to build the compiler; linking generated IR that uses runtime
C calls requires `-lffi`.


Extern calls use `extern.name(...)`; declarations are file-scoped. The package
and backend tests cover sibling-file isolation, imports not exposing externs,
redeclaring a C function in multiple files, ordinary functions and local variables
with the same name, missing qualifiers, and rejection of nested declarations.

## Type constraints and type switches

`ctest --output-on-failure -R type_constraints_test` checks native and optimized
execution of generic-family parameters, nested constraints, generic function and
struct constraints, type-switch narrowing, first-match ordering, default cases,
small scalar type identity, and switch control flow. It also checks rejection
of incompatible specializations, unresolved case types, family member access,
and value-parameter constraints. Buffer coverage includes bare and element-typed
families, fixed and runtime-sized storage, unsized views, inline and indirect
boxed payloads, nested constraints, and rejection of unrelated types. This suite
uses the package build CLI.

## Implicit returns and diagnostic severity

`fallthrough_test.py` checks implicit zero returns at `-O0` and `-O2`, including
scalars, pointers, aggregates, arrays, generic functions, methods, multiple
results, and named results. It verifies warnings in check/build/IR modes,
reachability through branches, loops, switches, and gotos, and one warning per
generic declaration. It checks colored warning/error headers, excerpts, carets,
plain redirected output, and `NO_COLOR` precedence across frontend/backend
source diagnostics. Warnings must leave compilation and execution successful.

## Array addition

`array_addition_test.py` checks concatenation, single-element append/prepend,
chaining, `+=`, empty inputs, type inference, generic returns, and left-to-right
operand snapshots. Native, `-O0`, and `-O2` runs verify recursive value copies for
nested arrays, strings, structs, fixed-buffer elements, pointer elements, and
`any` arrays. Semantic checks reject incompatible operands and immutable targets.

`io_backend_test.py` exercises the IO library fixture at O0/O2: printing,
format substitutions and escapes, signed integer boundaries, float rounding,
`i16`/`f32` arithmetic and C varargs, value switches and fallthrough, homogeneous
`any[]` arrays, mixed `...any` packs, array-family switches, and diagnostics.
Boolean and fixed/runtime array formatting checks include empty, nested, mixed,
and `.array()` values. Struct formatting checks cover `{key: value}` output,
empty and generic structs, nested fields and arrays, and format substitutions.
The suite supplies its own trusted `reflect` fixture through `OCEAN_SRC`. The fixture mirrors `../ocean-src/io/io.oc` so the suite
is self-contained.

`mixed_arrays_test.py` checks `mixed any[]`, homogeneous and mixed named-interface
arrays, empty-interface equivalence, method signatures and pointer method sets,
embedded interfaces and unions, generic receiver substitutions, array casts and
type switches, variadic packs, recursive copies, and runtime conformance checks.
Valid programs run at both `-O0` and `-O2`.

Mixed array constraint tests explicitly accept homogeneous interface arrays and
conforming concrete arrays in generic arguments and type switches, and reject
concrete arrays whose element type does not satisfy the interface.

`type_definitions_test.py` covers distinct `type Name Underlying` declarations and
transparent `type Name = Underlying` aliases. It checks raw-buffer casts, numeric
operations, aggregate layout and value copies, forward/local/imported types,
recursive pointers, alias chains and generic struct aliases, methods and method
sets, generic constraints, native varargs, boxed identity, cycle diagnostics, and
rejection of implicit interchange. Valid programs run at `-O0` and `-O2`.


`collection_types_test.py` exercises the syntax and behavior in `types.txt` at
`-O0` and `-O2`: array length identity, inline fixed-array sizes, array addition,
checked and two-binding indexing, buffer construction and erased parameter
lengths, sized buffer/array casts, dynamic array mutation, independent nested copies,
`make`, `new`, `free`, recreation after free, immutable strings, and dependent
struct array fields. Parameterized array fields currently use indirect storage;
this suite tests their language behavior rather than asserting an inline ABI.
Earlier references to the old `T[]` inline payload layout describe the legacy
implementation; dynamic arrays now use length/capacity/data headers and heap payloads.


`new_allocation_test.py` checks `new(T)` at `-O0` and `-O2` for scalar widths,
floating-point types, booleans, strings, pointers, nested structs, fixed arrays,
buffers, dynamic arrays, zero-sized types, named types, aliases, and generic allocation.
It verifies independent allocations, writable pointers, heap lifetime across
function returns, string growth through pointers, zeroed aggregate padding, and
rejection of `new(void)`.


`string_dynamic_array_ownership_test.py` checks read-only strings, capacity/length
invariants, safe conversions between strings and dynamic arrays of bytes, and memory ownership at `-O0` and `-O2`.
A native allocator wrapper counts live `malloc`/`calloc` allocations and checks
every `free`, including invalid frees of static or stack storage. Repeated
assignment, self-assignment, concatenation, `*string` aliases, function returns,
variadic forwarding, nested dynamic arrays, struct fields, loop conditions/updates, and
C buffer calls must keep allocation counts bounded and reach zero after freeing
the final values.


Function/scope ownership regressions in `string_dynamic_array_ownership_test.py` also
verify automatic cleanup of ordinary local values, independent by-value
arguments from both stack values and heap pointees, recursive/early/aggregate
returns, and nested values on loop exits. Pointer variables, aliases, fields,
and returned pointers must leave the pointee alive for the caller. The generated
IR must contain no dynamic array tracking/cleanup registry or destructor callbacks;
cleanup is emitted as inline branches and frees. Allocator-wrapped native runs
at `-O0` and `-O2` verify bounded allocation counts and no invalid/double frees.

Dynamic array removal tests cover `remove(index)` and `pop()` return values,
order and length changes, bounds failures, invalid arguments, immutable string
rejection, and independent ownership of removed strings, records, and nested
arrays at `-O0` and `-O2`, including discarded results and heap receivers.

`runtime_array_test.py` checks immutable byte-size headers, `array<T>` value
parameters and returns, fixed-length signatures, generic constraints, variable
names, runtime bounds, checked indexing, concatenation, nested layouts, type
assertions, and constructor rejection at `-O0` and `-O2`. The ownership suite
also tracks allocations across copies and returns of arrays containing strings,
records, nested arrays, recursive calls, reassignment, and discarded results.

Dynamic array `.array()` regressions cover independent scalar and deep copies,
empty values, byte-size headers, temporaries, heap receivers, function returns,
global snapshots, boxed elements, string byte copies, bounds, and invalid
arguments. Allocation tracking checks cleanup and survival after freeing the
source at both `-O0` and `-O2`.

`reflect_test.py` exercises standard-library struct field snapshots, primitive
and named/generic/empty structs, declaration order and value parameters, private
fields, deep copies and allocation cleanup, pointer lifetimes, and installed
`bin/ocean` plus `src/reflect` lookup. It checks `-O0` and `-O2` and rejects
compiler primitive calls from ordinary code, copied packages, project-local
lookalikes, other standard-library packages, and escaping source/directory
symlinks. `OCEAN_SRC` points at a temporary trusted standard library.

`build_cache_test.py` verifies IR and native-object cache hits by counting Clang
invocations. It covers output reuse, `run`, checks, library objects, source and
dependency changes, standard-library edits, tool/compiler/environment identity,
checksums against Python SHA-256, corruption recovery, concurrent writers,
disabled/unavailable caches, error/output preservation, and symlink-safe cleanup.
All cache files are isolated in temporary directories through `OCEAN_BUILD_CACHE`.


`check_backend_test` verifies that checks perform lowering and native validation,
report missing native symbols, never execute the program, and publish no outputs
or cache artifacts. `lsp_semantic_test` exercises resolved hover, navigation,
references, binding-aware rename, imported symbols, incomplete member completion,
function-result completion, unsaved buffers, and UTF-16 positions. The build cache
suite counts native compiler invocations to verify per-package object reuse,
content invalidation, corruption recovery, and concurrent publication.

`enums_test.py` exercises tagged enums at `-O0` and `-O2`: unit, tuple,
record and fixed payload variants; checked extraction; tuple discards; named
record patterns; exhaustive/default matches; nominal identity; imports;
generic and boxed transport; immutable record fields; and payload ownership.
It also covers generic enum parameters and constraints, nested and distinct
specializations, aliases, imported generic enums, generic functions, and concrete
`any` payloads. It compiles and runs the examples in `testcode/enums.oc`.

## Standard-library assembly

`inline_asm_test.py` checks selectors, raw assembly text, scalar `${name}`
read/write operands, name qualification and diagnostics, and native execution at
O0/O2. `standard_library_asm_test.py` uses `asm_backend_probe` to pass real parsed
standard-library blocks through the backend emitter and cross-assemble all seven
supported OS/architecture combinations at O0/O2. It compares edge cases and seeded
binary64 inputs against `snprintf` and runs the Windows x64 call sequence against
native `ms_abi` test shims on Linux. Foreign objects are assembled, not executed;
macOS/Windows and foreign-architecture behavior still needs native CI coverage.
The `io.oc` fixture mirrors the standard-library implementation without externs.

## Scanners and native files

`io_scanner_test.py` installs the IO, OS, and reflection fixtures into a temporary
standard-library tree. It runs delimiter scans, partial/fixed-size reads, EOF,
leading whitespace, binary data, long-line compaction, all FileMode values, Unicode
paths, shared Close state, permission checks, and native failures at O0/O2.
Live pipes remain open during readiness tests so a blocking HasNext call fails
with a timeout. All platform assembly blocks cross-assemble using the real parser
and backend probe. Windows x64 calls additionally run against ms_abi file/pipe
shims, with deliberately short native reads and writes. Foreign OS/architecture
objects and Windows console input still require native platform CI coverage.

The file suite also checks Binary mode rejection by NewScanner, all byte values
including 0xFF, static array return types, n-slot zero padding, whole-file reads
that preserve cursor/EOF state, inclusive seek bounds, shared EOF observations,
and scanner buffer invalidation after Setpos. The Windows shims cover
GetFileSizeEx and SetFilePointerEx; all native seek blocks cross-assemble.

## Built-in Result and numeric parsing

`result_parse_test.py` exercises the compiler-owned Result enum without imports,
all three unwrap methods, caller-attributed panics, generic/managed payloads,
local shadowing, integer overflow, strict parsing syntax, and exact binary64
conversion at O0/O2. Seeded finite floats and exact neighboring-value midpoints
are checked bit-for-bit against Python, including signed zero and subnormals.
The semantic LSP suite checks builtin completion/hover and prevents renaming or
navigating to nonexistent builtin source files. `io_parsing.oc` mirrors the
standard-library parser source and contains no native parsing dependency.

`recoverable_io_test.py` checks Result-based file, scanner, formatting and output
errors at O0/O2, including native failures, short EOF payloads, stdout write
errors, and Windows read/seek fault injection. Close and Setpos failures remain
panics; Readall restores cursor and EOF state on recoverable read failures.

Integer conversion coverage in `semantic_test.c` checks every signed/unsigned
width pair for range-preserving implicit calls and explicit casts, plus narrowing
in assignments, returns, generic calls, collection methods, and pointer/array
storage. Fitting literals retain contextual typing; out-of-range literals and
integer expressions require `->` for narrowing. `backend_test.py` verifies signed
and unsigned widening, mixed-width arithmetic, and explicit truncation at runtime.

## Deferred calls and recovery

`defer_test.py` checks argument and receiver capture, loop/block registration,
LIFO order, explicit and implicit returns, named results, panic propagation,
returned/forwarded panic exclusions, and direct recovery at `-O0` and `-O2`.
Run `ctest -R "defer_test|panic_test" --output-on-failure` after building.

## Function literals and closures

`closure_test.py` compiles and runs function literals at `-O0` and `-O2`.
It covers callable parameters/results, structural signature checking, escaped
and shared captures, recursive closures, generic specializations, per-iteration
locals/range bindings, struct/array/any storage, deferred recovery, empty callable
panics, lexical types, and callbacks crossing package boundaries.
Run `ctest -R "closure_test|defer_test|panic_test" --output-on-failure` after building.

## Value equality and string ordering

`equality_test.py` checks struct/array/buffer/enum/closure equality, boxed dispatch,
unconstrained generic searches, nested owned values, numeric edge cases, operand
side effects, and lexicographic string ordering at `-O0` and `-O2`.
Run `ctest -R "equality_test|type_lists_test|backend_test" --output-on-failure` after building.

`maps_test.py` exercises built-in `map[K]V` literals and zero values, insertion,
replacement, collision-heavy growth and deletion, snapshot range loops, recoverable panics for missing
single-value reads, zero/false two-binding lookup, structural equality, generic/boxed transport,
nested ownership, mixed interface keys/values, and conformance diagnostics and
runtime checks at both `-O0` and `-O2`.

`reachability_test.py` checks unused-function pruning, initializer/recursive/deferred
and function-value dependencies, library entry-point preservation, reflection
(including nested structs, arrays, and Result payloads), rejection of the removed
reflection-omission option, and native section garbage-collection flags.

`build_cache_test.py` verifies source/dependency action keys (including source edits
that leave package IR byte-identical), direct per-package modules, warm hits before
emission, reuse across new callers and unrelated package/type additions, stable
native identities for boxed structs/generics/closures/defers/maps, toolchain and
environment invalidation, corrupt artifacts, concurrent writers, and unavailable
caches. Native package objects retain ordinary functions; executable linkers prune
unreachable sections. Merged `--emit-llvm` remains an inspection artifact.

`interface_dispatch_test.py` exercises boxed method calls through parameters,
returns, mixed arrays, maps and embedded interfaces, pointer mutation and value
receivers, explicit generic specializations, variadic packing/spread, string
ownership, deferred calls, optimized LLVM, and package builds with cache reuse.

`ocean_test_test.py` exercises `ocean test`, internal/external test packages,
normal-build exclusion, fatal/panic/cleanup behavior, tang reports, JSON events,
report files, and timeout handling against the matching standard library.

`tests/slicing_test.py` checks half-open slices of arrays, buffers, and strings,
independent nested storage, bound evaluation, generic results, and invalid bounds.

`check_backend_test.py` also verifies that `check` and `build --check` validate
same-package and external test sources and test entry signatures without executing
tests, while normal builds and dependency loading exclude test sources.
`lsp_test.py` covers unsaved same-package and external test diagnostics and fixes.

`ocean_test_tui_test.py` validates the Ocean `term/tui` test frontend with a
pseudo-terminal: incremental completion, nested tests, resize/input, NO_COLOR,
Ctrl+C, external signals, timeout, renderer crashes, and noninteractive modes.

`ocean_test_parallel_test.py` checks the default 16-worker pool, explicit worker
counts, OS thread reuse, shared-queue scheduling, worker replacement after fatal
failures/skips, async subtests beyond the pool limit, and parent cleanup ordering.
