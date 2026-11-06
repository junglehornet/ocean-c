---
name: ocean
description: Write, check, run, and build Ocean (.oc) programs using the Ocean compiler, module and package system, native tooling, and current language syntax.
---

# Coding in Ocean

Use this skill for Ocean programs and libraries. Ocean is actively evolving;
confirm version-sensitive behavior against the target compiler. In its checkout,
`README.md`, `types.txt`, `testcode/`, and executable tests under `tests/` provide
examples. Parser acceptance alone does not establish backend support. Prefer
`ocean check` to validate complete programs. This file is self-contained so it can
be downloaded and installed as `ocean/SKILL.md` in an agent's skills directory.

## Tooling and build workflow

An installed compiler is named `ocean`; `ocean-c` accepts the same interface.
From a compiler checkout, build it with a C23 compiler, CMake, and libffi development
headers. Clang must be on PATH to check, link, or run Ocean programs. LLVM development
libraries are unnecessary: Ocean emits textual LLVM IR and invokes Clang.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
# Run the resulting build/ocean, or install:
cmake --install build --prefix "$HOME/.local"
```

Installation places the executable in `bin/` and standard-library sources in
`src/`. CMake's `OCEAN_STANDARD_LIBRARY_SOURCE` selects the library source tree
(default: sibling `../ocean-src`). `OCEAN_SRC` overrides the runtime source lookup;
otherwise it is `<executable-directory>/../src`. Keep standard-library source
files and package directories intact; do not precompile the library.

In an Ocean project:

```sh
ocean mod init codeberg.org/example/application
ocean check .
ocean run . arg1 --verbose
ocean build . -o application
ocean build --emit-llvm . -o application.ll
ocean build --check .
ocean check --lsp
ocean clean --cache
```

A location is a directory or a file selecting its entire containing package.
`check` defaults to the current directory and checks imports, semantics, backend
lowering, LLVM verification, and native linking without running or publishing an
output. Fix errors and inspect warnings before using `run` for behavior checks.
`build` produces a native executable for a root `main` package; other packages
produce object files and cannot be run. Default output is the directory name for
executables, `<package>.o` for libraries, and `a.ll` for emitted LLVM. `-o` overrides it.

`run` executes a private temporary build with the current working directory and
inherited standard streams, returns the program's status, and cleans up afterward.
Compiler flags `-o`, `--check`, and `--emit-llvm` are incompatible with `run`;
use `ocean run . -- -o literal --check` to forward those strings as program arguments.

Configure an editor's LSP client with command `ocean` and arguments
`["check", "--lsp"]`. It supports diagnostics with unsaved buffers, completion,
hover, navigation, references, and rename. Protocol positions use UTF-16.

Build artifacts are cached under `~/.cache/ocean-build`; override with
`OCEAN_BUILD_CACHE` or set it to `off`. Sources and compiler identity invalidate
cache entries; semantic checking still runs and executables relink. Cleaning
this cache does not remove downloaded modules.

## Modules and packages

Every `.oc` file starts with `package name;`. All `.oc` files in one directory
form one package, share ordinary declarations, and must agree on the package name.
Put independent programs in separate directories. Only a root package named
`main` gets an executable entry point: `func main()` or `func main() int`.
Imported functions named `main` are ordinary functions.

`ocean mod init [module-path]` creates `ocean.mod` in the current directory,
defaulting to the directory name, and preserves an existing file. Example:

```text
module codeberg.org/example/application

ocean 0.1

require (
    codeberg.org/example/collections v1.2.3
)
replace codeberg.org/example/collections => ../collections
```

Module subdirectories are separate packages. `require` selects Git tags or
branches. `replace` accepts a local module directory with its own `ocean.mod`, or
another module and version. Single-line and parenthesized directives, quoted
paths, and `//` comments work.

```ocean
package main;
import "codeberg.org/example/application/helper";
import "codeberg.org/example/collections/list" as lists;

func main() int { return helper.Answer(); }
```

Imports resolve within the module, the standard-library source tree, or external
Git repositories over HTTPS. Module-relative imports such as `"helper"` work.
The default qualifier is the imported package's declared name; `as alias`, dot
imports, and blank imports are supported. Import bindings are file-scoped.
Capitalized names are exported (including types, globals, and receiver methods);
lowercase names remain package-private. Extern declarations are file-scoped and
are not exported. Import cycles and conflicting declarations fail.

External modules cache under `~/.cache/ocean/modules/<module>@<version>`;
`OCEAN_MOD_CACHE` overrides this path. Undeclared conventional
`host/owner/repository/subpackage` imports use the repository's default branch.
There is no module proxy or Go vanity-URL discovery. Prefer declared versions
for reproducible dependencies. Dependencies initialize before importers; files
initialize in filename order, then declaration order. Globals initially have
zero values, even when their explicit initializer has not run yet.

## Core syntax

Statements end in `;` unless they end in a block. Use braces and parenthesized
control-flow conditions. Variables use `name: Type`, parameters and fields use
`name Type`, and inferred declarations use `:=`. Externs and named results have
different, type-first ordering.

```ocean
package main;

struct Point { x, y int; }
func twice(value int) int { return value * 2; }
func sum(values ...int) (int, int) {
    total := 0;
    for (i, value := range values) { total += value; }
    return total, len(values);
}
func main() int {
    count: int = 3;
    values: int[] = {1, 2, 3};
    total, length := sum(values...);
    if (length != count) { panic "unexpected length"; }
    for (i := 0; i < count; i++) { total += twice(i); }
    return total - 12;
}
```

Types: `int`/`i64`, `i32`, `i16`, `i8`, `uint`/`u64`, `u32`, `u16`, `u8`,
`float`/`f64`, `f32`, `bool`, and `string`. Ocean `int` is signed 64-bit,
not C `int`. Strings are immutable bytes; indexing reads a signed byte and `len`
counts bytes. Character literals decode Unicode scalars. Identifiers support
Unicode; exported names begin with Unicode uppercase letters.

Integer literals (including negative literals) automatically convert in float contexts,
including typed initializers, assignments, arguments, returns, arithmetic, and
comparisons such as `myFloat == 0`. Integer variables and integer expressions
require an explicit cast; `value := 0` still infers `int`.

Use `value -> Type` for casts and parentheses for compound targets, such as
`raw -> (*Point)` or `value -> (pkg.Type)`. Arithmetic, bit operations,
comparisons, compound assignments, `++`, `--`, short-circuit `&&`/`||`, and
`condition ? yes : no` work. `sizeof(value)` does not evaluate its operand;
`zero(Type)` takes a type and produces its default value. Uninitialized locals
are defaulted. Explicit returns avoid implicit zero-return warnings.

Multiple results unpack with `a, b := f();` or simultaneous assignment; `_`
discards a result. Named results use `(int total, count)` and allow `return;`.
A final `values ...T` parameter accepts variadic values; use a final `items...`
to expand an array. `...any` permits heterogeneous arguments.

Control flow includes `if`/`else`, `for`, range loops, `switch`, `break`,
`continue`, and explicit `fallthrough`. A one-binding range receives the index,
not the element. Type switches use `switch (v := value.(type)) { case int: ... }`.
Labels and `goto` are function-local and cannot enter nested scopes or skip
in-scope declarations. `panic "message";` terminates with a diagnostic and call
trace. `return panic "message";` reports at the caller. Use
`forward panic return helper();` for a single statement, or
`forward panic { first(); second(); }` for a block, to forward returned panics
from those calls to the current function's caller. Only failures originating
from `return panic` participate: `forward panic { panic "hi"; }` still reports
at `panic "hi"`. Consecutive forwarding callers move the location outward;
nested forwarding blocks in one function do not skip extra frames. Successful
calls behave normally; messages and full traces are preserved. A wrapped
declaration remains visible in its enclosing scope. Forwarding does not catch
panics or allow execution to continue.

## Collections and ownership

Choose the collection explicitly; these types have different storage and APIs:

| Form | Meaning |
| --- | --- |
| `T[n]` | Value array with length in its type when constant; immutable `.size` is payload bytes, `.data` exposes raw elements. |
| `array<T>` | Length-independent array value/constraint; accepts `T[n]` of any length. |
| `T[]` | Heap-backed dynamic array with length, capacity, and data; grows with mutation methods. |
| `buffer<T>[n]` | Raw unchecked storage with lexical length metadata. |
| `buffer<T>` / `buffer<T>[]` | Raw pointer view, without stored length or ownership. |
| `string` | Immutable heap-backed bytes; capacity equals length. |

```ocean
fixed: int[3] = {1, 2, 3};
counted := int[...]{4, 5};       // int[2]
inferred := {6, 7};             // int[2], not a dynamic array
n := 4;
runtime: int[n];
items := make(int[]);
items.append(2, 3);
items.prepend(1);
items.add(1, 8, 9);
removed := items.remove(1);
last := items.pop();
snapshot := items.array();       // independent array<int>
value, ok := fixed[3];           // zero and false, rather than a trap
```

Ordinary array/dynamic-array indexing checks bounds. Raw buffer indexing does
not. Buffer lengths are erased at function boundaries: pass an explicit count;
a sized cast can restore lexical metadata. `len` is element count; `.size` is
payload bytes. Dynamic arrays expose `.length` and `.capacity`. Strings expose
byte counts and `.array()`, but reject element writes and mutation methods.

Construct arrays using `x: T[n]` or `T[n]{...}`. For compatibility, expression
`T[n]` without a literal body creates a dynamic array. `array<T>` is not a
constructor: no `array<T>{...}`, uninitialized `array<T>`, `zero(array<T>)`, or
`new(array<T>)`. Construct `T[n]` and pass it to `array<T>` parameters instead.
Bare `array` is invalid; `array<any>` matches arrays, not dynamic arrays/buffers.

Typed literals are required in call arguments and return expressions:
`consume(int[]{1, 2});`, not `consume({1, 2});`. Assignment destinations supply
literal types. Fixed array/buffer literals zero-fill omitted elements. Struct
literals accept positional or keyed fields. `+` concatenates collections or
adds a compatible element; `+=` replaces the destination. `.data = {...}` copies
into existing payload storage rather than rebinding the collection.

Value assignments, arguments, and returns copy owned data recursively. Mutating
a copy does not mutate the original. Scope exit and reassignment clean up owned
strings/dynamic arrays, including nested values, without garbage collection.
Pointers and buffer views copy addresses. Use `*T`, `&value`, and `*pointer` for
shared access; `*T[]` points to a dynamic array while `T*[]` holds pointers.
`new(T)` returns an explicitly managed `*T` initialized to `zero(T)`;
`free(pointer)` releases owned contents and the allocation, without following
pointer fields. Pointer scope exit never frees the pointee. `free(items)` can
release dynamic storage early; recreate it before further use.

Explicit `null` is distinct from a default zero-address pointer. Initialize list
terminators explicitly: `p: *Node = null;`. `p := null` cannot infer a type.
Strings compare byte contents with `==`/`!=`. String-to-`i8[]` conversions copy.
String `.data` in a call is an isolated temporary buffer valid for that call;
create a named mutable byte array for persistent C access.

## User types and generics

```ocean
struct Box<T> { value T; }
func identity<T>(value T) T { return value; }
func (b *Box<T>) Set(value T) { b.value = value; }
func (b Box<T>) Pair<U>(other U) (T, U) { return b.value, other; }
struct Matrix(height, width int) { data int[height][width]; }
type UserID int;                 // distinct identity
type ID = UserID;                // alias, same identity
```

Generic arguments use `<...>`; supply explicit function/method arguments:
`identity<int>(42)`, `box.Pair<bool>(true)`. Type inference for generic calls is
unsupported. Missing constraints default to `any`; receiver type parameters
bind from the struct or enum. Value receivers copy; pointer receivers borrow addressable
values automatically. Struct value parameters are immutable fields preceding
ordinary fields, initialized once in literals such as `Matrix{2, 3}`.

A defined type needs explicit casts to/from its underlying type; aliases do not.
Generic constraints include interfaces, unions, `~T` underlying types, and struct
families (`Box` versus `Box<int>`). Boxed `any` values support type assertions
and type switches. `mixed any[]` allows heterogeneous elements; ordinary
interface arrays require one concrete element type. Interface method calls
require narrowing to a concrete type first.

```ocean
enum Result<T> { value(T), error(string), }
func unwrap<T>(result Result<T>, fallback T) T {
    match (result) {
        Result.value(v) => return v;
        Result.error(_) => return fallback;
    }
    return fallback;
}
// Inside a function:
// result := Result<int>.value(42);
// extracted := result.(Result<int>.value);
```

Enums are nominal tagged variants, not integers. They support unit, tuple,
record (`Click{x, y int}`), and fixed payload variants. Payloads are immutable;
wrong-variant extraction traps. Use `match`, not enum type-guard switches;
`EnumName => statement;` is the default arm. Generic constructors require type
arguments; patterns can infer them from the subject.
Enums support value and pointer receiver methods, including generic receivers
(`func (r Result<T>) unwrap() T`). Method names cannot collide with variant
names or named payload fields. Methods are called on values; `Result.unwrap(v)`
is not a variant constructor or match pattern.

## Native calls and current limits

```ocean
extern i32 puts(buffer<i8> text);
extern i32 printf(buffer<i8> format, ...);
func hello() { extern.puts("Hello, Ocean!\0".data); }
```

Extern declarations use return-type-first and parameter-type-first syntax,
end with `;`, and are called as `extern.name(...)`. Redeclare in each calling
file. Match the host C ABI: use `i32` for typical C `int`, raw buffers for C
strings, and explicit NUL terminators when required. Ocean collection headers
are not C pointers; pass `.data`. Use `*void` for C void pointers; conversion
back to a typed pointer requires a cast. Struct-by-value extern calls are
unsupported; use pointers. C variadic forwarding uses libffi; manually linking
saved IR/objects using it requires `-lffi`.

Do not assume Go or C features exist merely because syntax looks familiar.
Current unsupported backend features include closures, function values, bound
method values, interface method dispatch, operator dispatch, and `defer`.
Parameterized array-field layout and dynamic array ABI are compiler-specific;
do not rely on their layout at native boundaries. Validate unfamiliar features
with a minimal package and the target compiler before building on them.
