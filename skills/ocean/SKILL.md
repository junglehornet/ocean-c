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

Native builds emit LLVM directly into one module per source package and a shared
support module for globals, startup, concrete specializations, and runtime helpers.
Each source package compiles all its ordinary functions, so the same cached object
can serve callers using different functions. Executable linking removes unreachable
functions and data using function/data sections (`--gc-sections`, or `-dead_strip`
on macOS). Non-main library builds retain callable functions for downstream users.
`--emit-llvm` remains a single merged inspection file, with reachable executable
functions, rather than the native package-module format.

Use `ocean build --static . -o application` (or `ocean build -s .`) to
statically link a main package executable. This passes `-static` to Clang and
requires a host toolchain with static system libraries (and static libffi when
needed). Unsupported targets or missing static libraries cause a linker error.
`ocean run --static .` (or `ocean run -s .`) uses static linking for its
temporary executable too. The flag cannot be combined with `check`, `--check`,
or `--emit-llvm`, and is rejected for non-main packages. Cached objects are
reused and relinked. Use `--` to forward literal `--static` or `-s` program arguments.

`run` executes a private temporary build with the current working directory and
inherited standard streams, returns the program's status, and cleans up afterward.
Compiler flags `-o`, `--check`, and `--emit-llvm` are incompatible with `run`;
use `ocean run . -- -o literal --check` to forward those strings as program arguments.

Configure an editor's LSP client with command `ocean` and arguments
`["check", "--lsp"]`. It supports diagnostics with unsaved buffers, completion,
hover, navigation, references, and rename. Protocol positions use UTF-16.

Native objects are cached under `~/.cache/ocean-build`; override with
`OCEAN_BUILD_CACHE` or set it to `off`. Package keys hash source contents/paths,
transitive dependency sources, compiler/toolchain identity, host, working directory,
and environment; emitted LLVM is not a native cache input. Packages compile all
ordinary functions and executable linkers prune unused functions/data. Shared
support (globals, startup, specializations, and type-dependent runtime helpers)
uses a whole-program source key. Full cache hits skip backend LLVM generation and
native compilation, then relink. Rebuilding support still lowers the graph to
rediscover runtime/type requirements even for cached packages. Dependency edits
conservatively invalidate their importers. Parsing and semantic checking still run.
`--emit-llvm` keeps its separate source-keyed merged output cache, and `check`
validates direct package modules in temporary files. Cleaning the build cache does
not remove downloaded modules.

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

Integer variables convert implicitly only when the destination represents the full
source range: `i8` to `i32`, `u8` to `u16`, and `u8` to `i16` work. Narrowing,
signed-to-unsigned conversions, and unsigned-to-signed conversions without an
extra destination bit require `value -> Type`. This applies to arguments,
initializers, assignments, returns, and collection elements. Fitting integer
literals may directly initialize or supply narrower types (`small: i8 = 42`);
integer expressions and variables are checked by type, not their current value.
Pointer and collection element storage types must match; scalar widening does
not convert an `i8[]` into an `i32[]`. Mixed-width integer arithmetic promotes
to 64 bits; fitting literals use the other operand's type. Mixed signed/unsigned
arithmetic needs a cast if no signed 64-bit type can represent both ranges.

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

Function literals use `func(parameters) resultType { body }`; omit the result
type for a function with no results. Their types use the same signature without
the body: `func(int) int`, `func()`, or `func() (int, bool)`. Parameter names do
not affect type identity. Parameter and result types, their storage widths,
and the variadic marker must match. Functions can be assigned to variables,
passed as arguments, returned, stored in structs or arrays, and boxed in `any`.
Named functions are values too; specialize generic functions explicitly, such
as `identity<int>`, before passing them as values. Function literals can use
outer generic type parameters but cannot declare their own type parameters.

Closures capture variables by reference. The surrounding function and sibling
closures share updates to a captured variable. Returning a closure preserves its
captures; copying a function value shares the same environment. Separate calls
to a factory create independent environments. Range bindings and locals declared
in a loop body have separate storage per iteration; a variable declared in a
loop initializer retains its ordinary loop scope. Captured storage currently
lives until process exit, when the runtime releases its owned values. Raw pointer
and buffer captures still borrow their pointees and backing storage.

Use a type alias for arrays of functions, such as `type Callback = func() int;`
then `callbacks: Callback[3];`. A suffix in `func() int[3]` belongs to the result
type. An uninitialized function value panics if called. Function values use an
Ocean closure ABI and cannot be passed directly as native C function pointers.
Bound method values and calls through interface methods remain unsupported.

```ocean
func map(data array<int>, closure func(int) int) int[] {
    result := make(int[]);
    for (i := 0; i < len(data); i++) {
        result.append(closure(data[i]));
    }
    return result;
}

func main() int {
    offset := 3;
    values := int[2]{1, 2};
    mapped := map(values, func(n int) int { return n + offset; });
    return mapped[0] + mapped[1]; // 9
}
```

Control flow includes `if`/`else`, `for`, range loops, `switch`, `break`,
`continue`, and explicit `fallthrough`. A one-binding range receives the index,
not the element. Type switches use `switch (v := value.(type)) { case int: ... }`.
Labels and `goto` are function-local and cannot enter nested scopes or skip
in-scope declarations. An unrecovered `panic "message";` terminates with a diagnostic and call
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

`defer cleanup(arg);` evaluates the receiver and arguments immediately and
runs the call when the enclosing function exits. Deferred calls run in reverse
registration order, including calls scheduled in loops and nested blocks.
Return expressions are evaluated before deferred calls; deferred calls can
change named results through pointers. Ocean's usual value-copy rules apply to
captured arguments; pointers retain their aliasing behavior.

`recover()` returns `any`. Called directly in a deferred Ocean function during
panic unwinding, it returns the panic message as a boxed `string` and stops the
panic. The panicking function finishes its remaining deferred calls and returns
its named results, or zero values for unnamed results. Execution resumes in its
caller. Outside that context, including an indirect helper call or a normally
returning function's deferred call, `recover()` returns `null`. A second call
after recovery also returns `null`; `defer recover();` does not recover a panic.
Use an inline deferred literal for recovery: `defer func() { message := recover(); }();`

An ordinary `panic` runs the function's defers, then its callers' defers as it
propagates. `return panic` skips the originating function's defers. A caller
that forwards that returned panic with `forward panic` also skips its defers;
the first caller that does not forward runs its defers and can recover. Further
propagation runs callers' defers normally. A direct panic inside
`forward panic { panic "hi"; }` runs local defers, as does an ordinary panic from
a called function. An unrecovered panic prints its diagnostic and exits with
status 1. A new panic in a deferred call starts unwinding from that call.

Compiler CLI, module/package discovery, and build configuration use the existing
commands. Callable values can cross Ocean package boundaries; ordinary standard-library
calls work inside literals, including deferred recovery functions. Deferred standard-library methods such as
`defer file.Close();` use the same rules as other concrete methods.

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
| `map[K]V` | Hash map with independently typed keys and values; interface sides always allow mixed concrete types. |

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
Strings compare byte contents with `==`/`!=` and support unsigned-byte
lexicographic ordering with `<`, `<=`, `>`, and `>=`. String-to-`i8[]` conversions copy.
String `.data` in a call is an isolated temporary buffer valid for that call;
create a named mutable byte array for persistent C access.

`==` and `!=` are available for every generic type parameter, including an
unconstrained `T`; no comparability constraint is required. Operands still follow
the language's type-compatibility and pointer/null rules.

Struct equality recursively compares stored fields, ignoring padding. Arrays
compare lengths and then elements recursively, including nested arrays and
structs. Capacity and allocation address do not affect array equality. Fixed,
runtime-sized, and dynamic arrays can compare when their element types match;
different lengths compare unequal. Fixed raw buffers compare their elements;
raw buffer pointer views and typed pointers compare addresses, without following
pointees. Enum value equality compares both the tag and the active payload;
comparison with a variant selector, such as `event == Event.Key`, tests only
the tag. Function values compare code/environment identity; copies of a closure
share identity, while separately created captured environments do not.

Boxed `any` and interface values dispatch equality using their concrete types.
Matching concrete types compare by the rules above; different types compare
unequal, except primitive numeric values retain the usual numeric conversions.
Floating-point equality is numeric even inside aggregates: positive and negative
zero are equal, and NaN is unequal to itself. Both operands are evaluated once,
left to right; the left value is preserved if the right expression mutates it.

Strings support all six comparisons: `==`, `!=`, `<`, `<=`, `>`, and `>=`.
Ordering is lexicographic over unsigned byte values, includes embedded NUL bytes,
and puts a shorter equal prefix first. It is case-sensitive and does not perform
locale collation or Unicode normalization. String constraints, including
`T string`, support ordering. An unconstrained `T` promises equality but does not
promise ordering or arithmetic.

```ocean
func indexOf<T>(data array<T>, value T) int {
    for (i := 0; i < len(data); i++) {
        if (data[i] == value) return i;
    }
    return -1;
}

struct Person { name string; age int; }

func main() int {
    people := Person[2]{{"Ada", 36}, {"Lin", 28}};
    if (indexOf<Person>(people, Person{"Lin", 28}) != 1) return 1;
    if (!("apple" < "banana" && "a" < "aa")) return 2;
    return 0;
}
```

Struct operator overloading is not supported. Use named receiver methods for
custom struct operations.

### Maps

`map[K]V` is a built-in hash map from keys of type `K` to values of type `V`.
Use `map[string]int{"Ocean": 1}` or a contextual initializer such as
`counts: map[string]int = {"Ocean": 1};`. A declaration without an initializer,
`zero(map[K]V)`, and `make(map[K]V)` create an empty map.

```ocean
counts := map[string]int{"Ocean": 1};
counts["Ocean"] += 1;
counts["compiler"] = 3;
value, present := counts["missing"]; // 0, false; reading does not insert
size := len(counts);                  // 2
for (key, value := range counts) {
    // key is string, value is int; iteration order is unspecified
}
delete(counts, "compiler");           // missing keys are harmless
```

A single-value read such as `value := counts[key]` panics if the key is absent.
Use `value, present := counts[key]` for a checked read: it returns the value
type's zero value and `false` when absent, or the stored value and `true` when
present. The boolean reports success. Neither form inserts an entry.
Indexed assignment inserts or replaces an entry. `len` counts entries. Range
loops bind keys, or keys and values, from a snapshot of the map. Map entries
cannot have their addresses taken, since insertion may move buckets.

Interfaces, including `any`, can be keys, values, or both. Maps always permit
mixed concrete types independently on each interface side; there is no
homogeneous-map mode and no `mixed` modifier. Every entry must satisfy its key
and value interface constraints; boxed values receive runtime checks.

```ocean
items := map[any]any{1: "one", "two": 2, true: false};
items["three"] = true;
```

Keys use Ocean equality: strings and aggregates compare contents, pointers
compare addresses, and boxed primitive numbers use existing numeric equality.
For example, keys `1` and `1.0` refer to the same entry in `map[any]V`.
Positive and negative floating zero are equal; NaN keys are unequal to themselves.
Copies of maps own independent keys and values recursively. Maps compare equal
when they have the same entries, regardless of insertion order or capacity.
Mutable map types require identical key and value types; map conversion does
not implicitly widen an existing concrete map to an interface map.

Array and pointer suffixes after `map[K]` belong to the value type:
`map[string]int[]` maps strings to dynamic arrays and `map[string]*int` maps
strings to pointers. Use an alias to make an array of maps. Maps support normal
parameters, results, struct fields, explicit generics, and boxed type assertions.
Map storage is managed automatically and has no public `.data` or capacity API.
No standard-library import or build/module configuration change is required.


## User types and generics

Interfaces contain either method signatures or one type list; the two forms
cannot be mixed. Method receivers are implied, parameter names are optional,
and methods can declare their own constrained type parameters:

```ocean
interface Arithmetic {
    func add(int) int;
    func subtract(other int) int;
    func multiply<T int | float>(T) float;
}
interface Number { int | float }
interface Integer { ~int }
func add<T int | float>(x, y T) T { return x + y; }
func sum(x, y int | float) int | float { return x + y; }
```

The `func` keyword is optional for interface method signatures. Type-list
interfaces allow an optional final semicolon. `int | float` matches exactly
those types (including aliases); a distinct `type ID int` requires `~int` to
match its underlying representation. `~int` does not include narrower types
such as `i8` or `i32`. Type lists also work directly in parameter lists and as
generic constraints. Smaller numeric value arguments can widen to an accepted
parameter type, for example an `i8` argument to `multiply<int>`. This does not
make `i8` satisfy an exact `int | float` generic type constraint.

Equality (`==` and `!=`) is available for every generic type parameter, including
unconstrained `T`. Other operators in generic or interface bodies must be valid
for every possible operand type, even if the function is unused. `int | float` permits arithmetic
and numeric comparisons; integer-only operators require integer-only lists.
An unconstrained `any` parameter does not promise arithmetic. Operands sharing
a generic parameter have the same concrete type; ordinary interface parameters
can hold different listed types, so their operations must support all pairs.
Named type-list parameters use boxed runtime dispatch; generic calls specialize
normally. Generic interface method implementations must match the required
signature and constraints, independently of parameter names. Generic methods
still require explicit type arguments at calls. Calls through a boxed method
interface still require narrowing to a concrete type first.

Interface operator requirements (`self * T`) and signed membership prefixes
(`-Type`, `+Type`, `=Type`) are no longer part of the language. Use type lists
for supported operations and named methods for custom struct behavior.


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
Current unsupported backend features include bound method values and interface
method dispatch. Generic function values require explicit type arguments.
Parameterized array-field layout and dynamic array ABI are compiler-specific;
do not rely on their layout at native boundaries. Validate unfamiliar features
with a minimal package and the target compiler before building on them.

## Inline assembly

Inside a function, write `asm architecture[/platform] { assembly }`. The
architecture is required: `x86_64`, `aarch64`, or `riscv64`. An optional `/linux`,
`/macos`, or `/windows` restricts the block to that host platform. Braces are
required; a trailing semicolon is optional. Unknown selectors are errors.

```ocean
func main() {
    asm x86_64/linux {
        mov rax, 39
        syscall
    }
    asm aarch64 {
        nop
    }
}
```

Ocean currently builds for the host through Clang. Nonmatching blocks are omitted
without assembling their bodies; selectors do not enable cross-compilation.
Every matching block executes in source order. x86-64 uses Intel syntax;
AArch64 and RISC-V use their standard LLVM assembler syntax. Bodies are raw
assembly, including newlines, directives, and literal `$` characters, with balanced
braces outside quoted strings and comments. Use `#` line comments on x86-64 and
RISC-V, and `//` on AArch64; `/* ... */` comments are also recognized.

Use `${name}` outside assembly strings/comments to read or write a mutable
scalar variable or pointer. Each placeholder is a memory operand, not its value
or an address register; use an instruction of the variable's storage width.
Initialize variables before the block. Operand limits differ by architecture:

| Architecture | Distinct operand names per block |
| --- | --- |
| `x86_64` | No compiler-imposed count limit; more than four are supported |
| `aarch64` | At most four |
| `riscv64` | At most four |

AArch64 and RISC-V currently reserve registers to address operands, which limits
how many distinct variables a block can reference. x86-64 can address stack
variables directly and does not have this four-operand restriction. Repeated
references to `${name}` share one operand: reading and then writing `${value}`
counts as one distinct variable, regardless of architecture. Collections,
structs, constants, and arbitrary Ocean expressions are not operands.

```ocean
value := 7;
asm x86_64 {
    mov rax, ${value}
    add rax, 3
    mov ${value}, rax
}
// value is now 10 on x86-64.
```

Ordinary integer and vector/floating-point registers are conservatively
clobbered, with read/write effects for operands, a memory barrier, and condition
codes where applicable. Preserve frame pointers, reserved registers, and special
machine state: x86-64 `rbp`; AArch64 `x18`/`x29`; RISC-V `gp`/`tp`/`s0`.
When operands are present, also preserve AArch64 `x9`–`x15` and RISC-V
`t3`–`t6`: the compiler uses them to address operands. Native calls may overwrite
these address registers, so save and restore them around calls.

Blocks enter with an aligned stack. Functions containing matching asm use no
red zone and are not inlined. Stack use and native calls must follow the platform
ABI, restore the stack pointer exactly, preserve operand-address registers, and
provide any required call space. Access operands before changing the stack and
after restoring it. Do not return, branch out of the block, or unwind through
it. Numeric local labels avoid collisions when blocks are repeated.

Instructions must be supported by the host's default Clang target; there is no
CPU-feature selection. Native `check`/build reports assembler errors through
Clang; `--emit-llvm` alone does not assemble instructions.

The standard-library `io` package has no `extern` declarations. Output uses
assembly system calls on Linux (x86-64, AArch64, RISC-V64) and macOS (x86-64,
AArch64). Windows x86-64/AArch64 assembly calls `GetStdHandle` and `WriteFile`;
native Windows executable links include `kernel32` (use `-lkernel32` when linking
saved IR manually). Other architecture/platform combinations report unsupported
output. Writes handle partial results and retry Unix interruptions; other output
failures return error Results. Float formatting is portable Ocean code with exact binary64
conversion and round-to-nearest, ties-to-even, independent of the C locale.
Default printed floats retain two decimal places. Compiler runtime and boxed
values still use native libraries/libffi; this does not make executables freestanding.

## Input scanning and files

Recoverable standard-library I/O failures return `Result<T>`. Use `.unwrap()` to
panic on an error, or inspect the error variant to recover. `Close()` and
`Setpos()` return no value and panic on failure; neither returns `Result<unit>`.

`os.Stdin()`, `os.Stdout()`, and `os.Stderr()` return `Result<os.File>`.
Stdin is readable; stdout and stderr are writable and cannot be scanned.
`os.Open(path string, mode os.FileMode)` also returns `Result<os.File>`.
Modes are `Read`, `Write` (create/truncate), `ReadWrite` (create without
truncating), `Append`, and `ReadAppend`. Binary counterparts are `Binary`
(read-only), `BinaryWrite`, `BinaryReadWrite`, `BinaryAppend`, and
`BinaryReadAppend`. `io.NewScanner(file)` panics for binary, closed, or
nonreadable files. Copies share native cursor, close state and EOF state.

```oc
package main;
import "io";
import "os";

func main() {
    file := os.Open("input.txt", os.FileMode.Read).unwrap();
    scanner := io.NewScanner(file);
    line := scanner.Scanln().unwrap();
    io.Println(line).unwrap();
    file.Close();
}
```

`io.Scan()` and `io.Scanln()` return `Result<string>` using a shared stdin
scanner. Scanner `Scan()`, `Scanln()`, and `Scanc(delimiter i8)` likewise return
`Result<string>`. They consume the delimiter and return an empty string when it
is immediately encountered. Scan stops at ASCII whitespace; Scanln stops at
newline or EOF; a preceding CR is retained. Scanc stops at the requested
byte or EOF. EOF is a successful result, including an empty string.

`scanner.Scann(n)` returns `Result<io.ScanResult>`, with `Bytes i8[]` and
`End bool` fields. It reads up to n bytes; End is true when EOF prevents filling
the request. Unlike File.Readn, the scanner buffer contains only bytes read.
Zero reads succeed without consuming input; negative sizes panic.
`HasNext()`, `HasNextln()`, and `HasNextc(delimiter)` return `Result<bool>`.
They inspect buffered input and check OS readiness without waiting for more
input, returning true for a complete delimited segment or remaining EOF data.
Readiness errors return errors rather than false. Readiness is a snapshot;
another reader can consume data before a subsequent scan.

## Byte reads, binary files, and file positions

| File method | Return value and behavior |
| --- | --- |
| `Read()` | `Result<int>`: unsigned byte 0–255, or -1 at EOF |
| `Readn(n)` | `Result<os.ReadResult>`: `Bytes array<i8>` of length n, `End bool` for early EOF; unused bytes are zero-filled |
| `Readend()` | `Result<array<i8>>`: current position through EOF, advances cursor |
| `Readall()` | `Result<array<i8>>`: entire file, preserves cursor and previous EOF state |
| `Readchunk(n)` | `Result<i8[]>`: up to n bytes; successful empty buffer at EOF |
| `Size()` | `Result<int>`: size in bytes, requires a seekable file |
| `PollRead()` | `Result<int>`: OS readiness information without blocking |
| `Write(text)` | `Result<int>`: number of bytes written |
| `Close()` | Closes the shared file; repeated close succeeds; native failure panics |
| `Setpos(position)` | Sets next byte offset from 0 through size inclusive; bounds and native failures panic |

```oc
file := os.Open("data.bin", os.FileMode.Binary).unwrap();
read := file.Readn(16).unwrap();
bytes := read.Bytes;
shortRead := read.End;
file.Setpos(0);
wholeFile := file.Readall().unwrap();
file.Close();
```

Read, Readn and Readend advance the shared position. Readall attempts to restore
the cursor even after a read failure; restoration failure returns an error and
invalidates buffered scanner state. Setpos also invalidates scanner buffers.
Another Read after EOF has already been reported panics. Negative sizes panic.
Ordinary access errors, closed files and native I/O errors return error Results,
not EOF. Failed operations can have consumed bytes or written part of the output.
Close failures are not retried because the OS may already have released the handle.

`File.IsOpen()`, `CanRead()`, `CanWrite()`, and `IsBinary()` return bool.
`os.FromDescriptor(fd, readable, writable)` adopts a native descriptor as a text
stream; negative descriptors panic. `File.NativeHandle()` returns the native
handle and panics when closed. Keep one scanner per stream and avoid mixing its
buffered reads with direct reads or writes.

`io.Print`, `Println`, `Printf`, and `Printfln` return `Result<int>` containing
the emitted byte count, including a newline where applicable. `io.Sprintf`
returns `Result<string>` and reports argument-count mismatches as errors.
`io.Sprint` remains a plain string conversion. Unix writes retry interruptions;
other native output failures return errors.

## Built-in Result and number parsing

`Result<T>` is available without imports or standard-library files. It is the
compiler-owned equivalent of:

```ocean
enum Result<T> { value(T), error(string) }
```

Construct values with explicit type arguments, for example
`Result<int>.value(42)` or `Result<float>.error("invalid number")`. Use normal
variant comparisons, extraction, and `match` to inspect either case. Its
lowercase variants and methods are public language built-ins, independent of
package capitalization rules; ordinary package members retain those rules.
A user-declared Result may shadow the built-in name and remains a distinct type.

| Method | value case | error case |
| --- | --- | --- |
| `unwrap() T` | Returns the payload | Returns a panic containing the stored error string, attributed to the caller |
| `unwrapOrElse(fallback T) T` | Returns the payload | Returns the provided fallback value |
| `unwrapOrZero() T` | Returns the payload | Returns `zero(T)` |

`unwrapOrElse` accepts a value, not a callback; its argument is evaluated with
normal function-call rules even when the Result holds a value. Payloads and
fallbacks retain ordinary value-copy and ownership behavior. `zero(T)` follows
Ocean's existing zero semantics, including a zero-address pointer distinct from
explicit `null`.

```ocean
package main;
import "io";

func main() {
    count := io.ParseInt("42").unwrap();
    ratio := io.ParseFloat("1.25e-2").unwrapOrElse(0.0);
    result := io.ParseInt("not a number");
    match(result) {
        Result<int>.value(value) => io.Println(value);
        Result<int>.error(message) => io.Println(message);
    }
    io.Println(count, " ", ratio);
}
```

`io.ParseInt(text string) Result<int>` parses the entire string as a signed
base-10, 64-bit integer. It accepts an optional leading `+` or `-` and one or more
ASCII decimal digits, including leading zeros. Both -9223372036854775808 and
9223372036854775807 are supported. Empty input, a sign alone, whitespace, digit
separators, base prefixes, fractions, trailing characters, and overflow produce
an error Result instead of panicking.

`io.ParseFloat(text string) Result<float>` parses a binary64 value from an
optional sign, decimal digits with an optional point, and an optional signed
`e`/`E` exponent. At least one digit is required; `.5`, `1.`, and `-1.25e2` are
valid. It also accepts case-insensitive `nan`, `inf`, and `infinity`, with an
optional sign. Whitespace, separators, hexadecimal forms, malformed exponents,
and trailing characters are rejected.

Finite decimal values round to the nearest representable float, with ties to
even. Subnormals are supported; sufficiently small magnitudes underflow to
signed zero. Finite overflow returns an error rather than infinity; explicit
`inf` is a successful value. Parsing is implemented in Ocean, independent of
libc parsing and locale. Current error messages distinguish invalid input from
out-of-range input. Only calling unwrap on an error turns that error into a
panic; unwrapOrElse and unwrapOrZero provide non-panicking fallbacks.

Use the matching updated compiler and io sources together. Result is embedded
in the compiler; ParseInt and ParseFloat are provided by the standard-library io
package. Existing compiler build/install and native library requirements remain
unchanged.

`io.Sprint` and the print/format functions support unsigned `uint`/`u64`,
`u32`, `u16`, and `u8` values as decimal strings across their full ranges,
including 18446744073709551615 for `uint`/`u64`.
