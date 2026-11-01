# Ocean compiler

Build with CMake, a C23 compiler, and libffi development headers:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Build or run a package directory (requires `clang` on `PATH`):

```sh
build/ocean build . -o hello
./hello
build/ocean run .
build/ocean build ./mySubPackage/
build/ocean run ./main.oc
```

A file location selects its containing package directory. Every `.oc` file in
that directory belongs to one package and shares Ocean functions, types, and variables.
Every file must begin with the same declaration, for example `package main;`.
Only the root package named `main` gets an executable entry point. Other packages
build to native object files; `run` rejects them. Imported functions named `main`
are ordinary functions and are never called automatically.

The compiler scans, parses, checks semantics, emits textual LLVM IR, then invokes
Clang for the current host. No LLVM development libraries are needed. Main
packages default to an executable named after their directory; other packages
default to `<package>.o`. `-o` overrides the destination. Compiler/linker failures
preserve previous output and cannot overwrite a source file in the package graph.

`ocean run` builds in a private hidden temporary directory, executes with the
current directory and inherited standard streams, and deletes the executable
and temporary directory afterward, including on runtime failures and handled
interrupts. The program's exit status is returned. Arguments after the location
or `--` are forwarded to the executable.

```sh
build/ocean build --emit-llvm . -o hello.ll
build/ocean build --check .
build/ocean check ./hello.oc
build/ocean check --lsp
```

`--emit-llvm` defaults to `a.ll`; `--check` validates the complete package graph through native code generation.

`ocean check [file-or-directory]` checks syntax and semantics for the package and
its imports, including backend lowering, LLVM verification, and native linking. Temporary artifacts are removed; no output is published and the program is never executed. It defaults
to the current directory and exits with a nonzero status on errors.

`ocean check --lsp` starts the Ocean language server over stdin/stdout using
[Language Server Protocol](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/)
framing. Configure an editor's Ocean language client to run `ocean` with arguments
`["check", "--lsp"]`. The server supports initialization, shutdown, full-document
synchronization, and scanner, parser, semantic, backend, and native diagnostics on open, change, and save. Open
buffers, including new unsaved files, are checked together with their package and
imports without modifying source files. Diagnostic and edit positions use UTF-16.
Semantic hover displays resolved types. Definition, declaration, type-definition,
and references follow symbol bindings across files and imports. Prepare-rename
and rename produce workspace edits, respect shadowed bindings, and reject invalid
names, collisions, and changes that make an imported export private. Completion
uses lexical scope and inferred receiver types, including fields, methods,
dynamic-array operations, and exported package members. Requests use unsaved
buffers, including incomplete member expressions.

Both `ocean` and `ocean-c` use this command interface. A standalone source file
can be passed as the location and must include a package declaration.

Initialize a module in the current directory:

```sh
build/ocean mod init github.com/yourname/yourproject
# Or use the current directory's name:
build/ocean mod init
```

This creates `ocean.mod` with the module name and `ocean 0.1`. An existing
`ocean.mod` is preserved. A directory containing `ocean.mod` is a module root;
its Ocean subdirectories are separate packages. For example:

```text
module github.com/example/application

ocean 0.1

require (
    github.com/example/collections v1.2.3
)

replace github.com/example/collections => ../collections
```

The `module` directive names the module. `require` selects dependency versions
(Git tags or branches); `replace` can select a local directory or another module
and version. Both single-line and parenthesized multi-line directive forms work;
quoted paths and `//` comments are accepted. A local replacement must have its
own `ocean.mod`.

```oc
package main;
import "github.com/example/application/helper";
import "github.com/example/collections/list";

func main() int {
    return helper.Answer();
}
```

Imports resolve to packages in the current module, the standard library at
`<executable-directory>/../src/<import-path>`, or external modules. Module-relative paths
such as `import "helper";` are also accepted. Import qualifiers default to the
imported package's declared name. Aliases (`import "helper" as h;`), dot imports,
and blank imports are supported. Import bindings belong to their source file;
package declarations are visible across all files. Only types, functions, and global
variables whose names start with a capital letter are exported to other packages.
For example, `helper.Answer()` is accessible to importers, while `helper.answer()`
is private to its package. The rule also applies to receiver methods, aliases,
interfaces, and enum constants. Dot imports introduce only exported names;
lowercase declarations remain accessible across files in their own package.
Extern declarations are file-scoped and are not exposed through package imports. Import cycles,
conflicting declarations, and mixed package names in a directory are errors.
Labels and `goto` remain function-local and cannot jump between files.
Source filenames in diagnostics are relative to the compiler's working directory,
including embedded runtime errors and call traces. Sources outside that directory
use `../` paths.

External modules are fetched with Git over HTTPS and cached under
`~/.cache/ocean/modules/<module>@<version>`. A required module path identifies
its repository; undeclared conventional `host/owner/repository/subpackage`
imports use the repository's default branch. Cached modules are reused. This
uses direct Git repositories, without a module proxy or Go vanity-URL discovery.
`OCEAN_MOD_CACHE` overrides the cache, and `OCEAN_SRC` overrides the standard
library source directory. `cmake --install build` installs the `ocean` command
under `bin/` and the standard-library tree under `src/`. The CMake cache option
`OCEAN_STANDARD_LIBRARY_SOURCE` selects the source tree to install (by default,
the sibling `../ocean-src` repository).

The standard `reflect` package exposes struct snapshots:

```Ocean
import "reflect";

struct Person { name string; age int; }
// Inside a function:
fields := reflect.Fields(Person{"Ada", 36});
// fields[0].Name == "name", fields[0].Value.(string) == "Ada"
```

`reflect.Fields(value any)` returns `array<reflect.Field>` in declaration order;
each `Field` has `Name string` and `Value any`. It includes private fields and
struct value parameters. Field values are independent deep copies, with normal
scope cleanup; pointer fields remain non-owning address copies. Non-structs
return an empty array, and pointers are not automatically dereferenced.
`reflect.IsStruct(value any)` distinguishes an empty struct from a non-struct.

Only the `reflect` package physically located under the configured standard
library root can call the compiler primitives implementing this API. A package
name, module replacement, copied implementation, or symlink escaping that
directory cannot enable them. Other standard-library packages also have no
intrinsic access. Ordinary Ocean code can import the public reflection API.
`OCEAN_SRC` is an explicit override of that trusted root for development.

Builds automatically cache generated LLVM IR and native object files in
`~/.cache/ocean-build`. Cache keys include the compiler executable, source
contents throughout the resolved import graph, source paths, and the working
directory. Object keys also include the IR, Clang identity, host target, and
environment. Changing a source or dependency invalidates the relevant action;
returning to previous contents can reuse the earlier result. Output filenames
and `ocean run` arguments do not invalidate the cache.

Generated LLVM is partitioned into one native object per source package and a
shared runtime/data object. Each object has an independent content key, so editing
a dependency reuses unaffected package objects. Changes to shared types, globals,
or generated specializations can also invalidate objects that depend on them.
Merged IR remains cached for `--emit-llvm`.
Parsing and semantic checks still run on every invocation. Executables are
always relinked against the current system libraries. Library `.o` builds and
`--emit-llvm` also reuse their cached artifacts; `ocean run` reuses the same
build cache. Failed artifacts are never published. Checksums and atomic writes
protect against corrupt entries and concurrent builds. An unavailable cache
silently falls back to compilation.

Set `OCEAN_BUILD_CACHE=/path/to/cache` to override its location, or
`OCEAN_BUILD_CACHE=off` to disable caching. `ocean clean --cache` (also
`ocean clean -cache`) removes the managed cache entries. Build and download
caches are independent: cleaning this cache does not remove downloaded modules.

Package globals are default-initialized before user initializers execute.
Imported packages initialize before their importers; within a package, files
are sorted by filename and initializers run in declaration order. A global
referenced before its initializer runs has its default value. Functions can
access globals declared in other files. Global arrays own copies with module
lifetime, including replacements assigned inside functions, and are released
at process shutdown. Explicit types are recommended for forward-referenced
global initializers. Raw buffer literals and addressed composite literals in
package initializers receive module-lifetime backing storage. Runtime-sized
raw buffer globals are rejected; use arrays for runtime-sized global storage.

The backend supports:

- `int`/`i64` (signed 64-bit), `i32`, `i16`, `i8`, `uint`/`u64` (unsigned 64-bit), `u32`, `u16`, `u8`, `f32` (single precision), `float`/`f64` (double precision), `bool`,
  and `string` (an alias for `i8[]`).
- Functions, recursion, forward calls, typed variadic parameters, multiple return
  values, named results, local variables, and lexical shadowing.
- Arithmetic, comparisons, integer bit operations, assignments, increments,
  numeric casts, ternaries, and short-circuit `&&` and `||`.
- `if`/`else`, ordinary and deferred-condition `for` loops, labels, `goto`,
  and labeled or unlabeled `break` and `continue`.
- Value and condition switches, source-ordered case tests, defaults, and explicit
  `fallthrough`, as well as type-guard switches.
- Fixed-signature `extern` functions resolved by the system linker. Declarations
  must match the external ABI: for example, C `int` is `i32` on the tested host,
  while Ocean `int` is `i64`. Use `buffer<i8>[]` for C character pointers;
  Ocean strings carry array headers.
- Fixed-size `buffer<T>[N]`, runtime-sized `buffer<T>[n]`, and unsized
  `buffer<T>[]` buffers, with contiguous storage and unchecked integer indexing.
- Stack-backed `T[]` arrays with an inline byte-size header, `T[count]`
  allocation expressions, `.size` and `.data` fields, and built-in `len(array)`.
- Native struct values, generic struct specialization, nested fields, pointers,
  composites, value copies, and structs in arrays, buffers, arguments, and returns.

Extern functions are file-scoped and must be called with `extern.name(...)`.
Declare each external function in every file that calls it. A declaration in a
sibling file or imported package does not make the external function available.
Ocean functions and local variables may use the same name independently.

Extern declarations use C-style ordering with Ocean type names:

```ocean
extern int write(i32 fd, buffer<i8>[] data, int count);
extern void srand(i32 seed);

func main() {
    extern.srand(42);
}
```

The return type precedes the function name, and parameter types precede their
optional names. Use `void` for no return value; either `()` or `(void)` denotes
no parameters. Declarations end with `;`. Ordinary Ocean functions keep
`func name(parameter type) resultType { ... }` syntax. C-style trailing `...` supports native C variadic calls. Extra arguments use
their Ocean types, with booleans promoted to `i32` and floats passed as doubles.
Use explicit casts when the C ABI requires a narrower integer type.

```ocean
func main() int {
    myBuffer: buffer<int>[4] = {1, 2, 3, 4};
    myBuffer[3] = 10;
    return myBuffer[3];
}
```

`string` is an immutable, heap-backed dynamic array of `i8`. Its header contains a
length, a capacity, and a data pointer; capacity always equals length.
`len(text)`, `text.length`, `text.capacity`, and `text.size` report its byte count.
String literals contain their decoded bytes without an implicit terminator.
Indexing reads a signed byte and checks bounds. Element assignment, mutable
element addresses, `append`, `prepend`, and `add` are rejected for strings.

```ocean
extern int write(i32 fd, buffer<i8>[] data, int count);

func main() {
    text := "Hello";
    first := text[0];
    text = "Hello, Ocean!";          // Replace the value, allocating new storage.
    text += "\n";                    // Concatenate and replace; never grow in place.
    bytes: i8[] = text;             // An independent, mutable dynamic array of bytes.
    bytes[0] = 'h';                 // Does not change text.
    extern.write(1, text.data, len(text));
    free(bytes);
    free(text);
}
```

Reassignment evaluates and copies the replacement before freeing the previous
header, payload, and owned nested values. This also applies through `*string`
and `*T[]`, including pointers returned by `new`. Self-assignment and `+=` are
safe. Passing or returning dynamic arrays/strings by value copies their backing memory.
Parameters, ordinary locals, named results, and owned nested values release their
storage when their stack scope ends, including on early returns and loop exits.
Expression temporaries are cleaned up as well. Explicit `free(value)` can release
a value early; subsequent scope cleanup does not free it again.

Pointer variables are non-owning: scope exit and pointer returns never free their
heap allocations or pointees. This includes pointers to dynamic arrays/strings returned
by `new`. Explicit `free(pointer)` releases the pointed-to value's owned
dynamic array/string storage and the allocation itself, without following pointer
fields. Failing to free a heap pointer can therefore leak memory.

Dynamic array/string cleanup is emitted directly into LLVM control flow. Temporary
ownership uses compiler-known stack slots and a private allocation prefix;
there is no runtime dynamic array allocation registry, reference counting, or garbage
collector. Returning a value copies it before the callee's cleanup executes.

Strings and mutable dynamic arrays of bytes can be assigned or explicitly converted to one
another by copying. Changes to a mutable dynamic array of bytes cannot affect its source
string. `text.data` is available in function arguments as an isolated temporary
buffer, so even an external function that writes into that buffer cannot modify
the original string. Such a buffer is valid only during the enclosing call;
create a named `i8[]` copy for persistent or mutable data access. C APIs that
require a terminator still need one explicitly, for example `"text\0".data`.

Fixed-size buffers accept positional initializers, with omitted elements
zero-initialized. Assignment, arguments, and returns copy a fixed-size array
unless an unsized buffer is expected, in which case the array's address is passed.

```ocean
func fill(data buffer<int>[], count int) {
    for (i := 0; i < count; i++) {
        data[i] = i;
    }
}

func main() {
    count := 8;
    data: buffer<int>[count];
    fill(data, count);
    view: buffer<int>[] = data;
    view[3] = 42;
    inferred: buffer<int>[] = {1, 2, 3};
}
```

A runtime size expression is evaluated once at the declaration. Runtime-sized
buffers allocate stack storage until their block exits; their elements start
uninitialized, and they cannot have initializers. Sizes must be positive; no
runtime size or index checks are generated. Element types must have fixed
storage sizes, so runtime outer dimensions can contain fixed-size inner buffers.

Unsized buffers are raw pointers without a stored length. `buffer<T>` is also
accepted as an unsized spelling. A local unsized buffer needs an initializer:
an existing buffer, an extern function returning a buffer pointer, or a literal
that allocates backing storage for its elements. Assigning or passing an unsized
buffer copies its pointer, so changes through it affect the same elements.
Neither an unsized view nor a runtime-sized buffer carries a length field.
All buffers have no methods or members, including no `.length`.

Built-in arrays use `T[]` types and `T[count]` allocation expressions:

```ocean
func main() {
    arr: int[] = int[5];
    arr[0] = 42;
    arr.data[1] = 7;
    count := len(arr);              // 5
    bytes := arr.size;              // 5 * sizeof(int), currently 40
    sameCount := arr.size / sizeof(int);
}
```

An array's allocated block starts with an Ocean `int` containing the payload
size in bytes, immediately followed by its inline element storage. The header
does not contain a data pointer or a separate element count. `len(arr)` reads
`arr.size` and divides it by the storage size of the element type. The `.data`
field exposes the inline payload as a raw `buffer<T>[]`. The `.size` field is an
immutable parameter; `.data` is mutable through copy assignments and element writes. Direct `arr[index]` reads and writes
check `0 <= index < len(arr)` before accessing data, including compound
assignments and increments. Checks derive their length from the byte-size header.

On an out-of-bounds index, the program prints the index, array length, filename,
one-based line and column, the source line with a caret, and an Ocean call trace
to standard error, then exits with status 1. Source snippets and call locations
are embedded in the executable and saved LLVM IR; the report works without
source files or debug symbols. The call trace starts at the failing access and
continues through its callers to `main`.

The `.data` field remains a raw buffer: `arr.data[index]` and all other buffer
indexing are unchecked.

Assigning to `.data` copies into its existing storage, rather than replacing the
field or changing `.size`:

```ocean
func main() {
    arr := int[4];
    arr.data = {1, 2};                 // {1, 2, 0, 0}
    source: buffer<int>[4] = {3, 4, 5, 6};
    arr.data = source;                 // Copies len(arr) items
}
```

A buffer source is read for exactly `len(arr)` items. Literal sources are checked
against the destination length before evaluating their items or writing data.
A literal that is too large produces a runtime error with source context and a
call trace, then exits with status 1. Short literals pad the remaining elements
with empty values: `0` for numbers, `false` for booleans, `""` for strings, and
empty arrays for array elements. All literal items are evaluated before copying,
so `arr.data = {arr[1], arr[0]};` can swap values. Compound assignments to `.data` are unsupported; use `=` to copy contents.
Taking a mutable address of `.size` is rejected. Use `arr.data` directly as the
raw buffer address instead of taking `&arr.data`.

Counts can be nonnegative runtime expressions and are evaluated once. Array
storage is allocated on the stack until its enclosing block exits; element
contents start uninitialized. `T[0]` and an uninitialized declaration such as
`empty: int[];` create an empty value with `len(empty) == 0`. Arrays have value
semantics: declarations, assignments, arguments, and returns copy the header
and elements. This recursively copies nested arrays and array fields in structs
and fixed buffers. Explicit pointers and raw buffer views copy their addresses
and continue to share their pointees.

Use `+` to concatenate arrays or add one compatible element at either end:

```ocean
first := int[]{1, 2};
second := int[]{3, 4};
joined := first + second; // {1, 2, 3, 4}
appended := first + 3;    // {1, 2, 3}
prepended := 0 + first;   // {0, 1, 2}
first += second;         // Replaces first with its concatenation with second.
first += 5;              // Appends one element to first.
```

Addition creates a new array and leaves both operands unchanged. Elements use
the same recursive value-copy rules as assignment; pointers and raw buffer views
still share their pointees. Operands are evaluated once, left to right, with the
left value captured before right-side effects. Array element types must match,
and appended/prepended values must be compatible with the element type.

For nested arrays, `int[][] + int[][]` concatenates the outer arrays, while
`int[][] + int[]` appends one inner array and `int[] + int[][]` prepends one.
Strings follow their existing `i8[]` representation: string addition concatenates
bytes, and a compatible byte value can be appended or prepended. Two raw buffers
cannot be concatenated with `+`.

Runtime-sized values are lowered indirectly. The caller supplies a hidden
pointer, and an Ocean callee allocates and copies its own array on its stack,
using the incoming runtime byte count. Mutating an array parameter therefore
changes only that local copy. Arguments are captured left to right before later
argument expressions can modify them.

Use `*T[]` and `&array` for explicit shared access:

```ocean
func change(copy int[]) { copy[0] = 99; }
func changeShared(array *int[]) { (*array)[0] = 7; }
func main() {
    a := {1, 2};
    b := a;
    change(a);             // a[0] and b[0] remain 1
    changeShared(&a);      // a[0] becomes 7; b[0] remains 1
}
```

`*T[]` is a pointer to an array; `T*[]` is an array of pointers. Array values
use stack storage. Replacement through an explicit pointer copies into
the owner's existing storage and must fit its current extent; oversize
replacement traps. Assigning a replacement directly in the owning scope can
allocate a new stack value.

Nonrecursive array-returning calls are lowered in the caller's frame, so returned
values do not point into a completed callee frame. Recursive functions can also
return runtime-sized arrays, including nested arrays and aggregates containing
arrays. Recursive calls transfer a deep copy through temporary heap storage; the
caller copies it onto its own stack and immediately frees the temporary storage.
Return expressions execute once, and explicit pointers retain their aliasing
behavior. Copies assigned into enclosing scopes retain
their storage until that enclosing scope exits.
Arrays and buffers are distinct: `len()` accepts arrays, while buffers still
require explicit counts. Arrays support argument expansion as described below; buffers do not.

`sizeof(T)` returns the storage size in bytes of a supported type;
`sizeof(value)` inspects its type without evaluating the value expression.
For an unsized array type, `sizeof(T[])` reports its fixed byte-count header;
`.size` reports its runtime payload size. Compiler-private pointers used to
lower these variable-sized values do not give `T[]` reference semantics.

Struct fields use `name type`, with grouped names sharing a type: `x, y int;`.

Struct value parameters are prepended to the declared fields in parameter
order for all structs, including generic structs. Concrete type arguments resolve
inside parameter and body field types. For `struct Config(int size, tag) { value int; }`, the complete field
order is `size`, `tag`, `value`; the first two fields are immutable. Composite
initializers supply those parameters once, while ordinary fields remain mutable.
Assignments, compound assignments, increments, and mutable addresses cannot
change parameter fields. An immutable buffer/reference binding still allows
writes to the elements it refers to. The frontend enforces immutability before
native code generation.

Native structs use ordinary named LLVM struct types with host alignment and
padding. Value parameters occupy the first fields; body fields follow in
declaration order. Generic **type** arguments exist only in the compiler: each
concrete instantiation resolves its field types and reuses a specialized layout,
without runtime type tags, descriptors, or extra fields.

```ocean
struct Box<T any>(int tag) {
    value T;
}

func make() Box<int> {
    return Box<int>{7, 42};
}

func main() int {
    box := make();
    copy := box;                    // Copies the fields by value
    copy.value++;
    return copy.value - box.value;   // 1
}
```

Here `Box<int>` reduces to an LLVM struct containing two `i64` fields: `tag` and
`value`. `Box<bool>` instead has `i64` and `i1` fields. Repeated uses of the same
instantiation share its LLVM type. `sizeof` includes the host's padding; buffers
are inline fields. Array/string fields use indirect storage internally and
are recursively copied as owned values.
Positional and keyed literals support nested composites and empty defaults.
Structs can be passed and returned by value, including variadic arguments and
multiple returns. Ordinary field writes, nested indexing, `&field`, dereferencing,
and pointer field access operate on the original storage.

Recursive ownership must use explicit pointers to break cycles; recursive
inline values are rejected. Runtime-sized inline buffer fields and struct value
argument application remain unsupported. Use `buffer<T>[]` or `T[]` for fields
whose data is stored separately. C functions can access structs through pointers;
passing or returning structs by value through `extern` requires additional C ABI
lowering and is rejected.

Composite literals use the assignment destination's type:

```ocean
func sum(values int[]) int {
    total := 0;
    for (i := 0; i < len(values); i++) { total += values[i]; }
    return total;
}

func main() {
    values: int[] = {1, 2, 3};
    buffer: buffer<int>[4] = {4, 5};   // Remaining items are empty values
    inferred := {6, 7};               // Infers int[] from its elements
    values = {8, 9};                  // Constructs a new two-item array
    total := sum(int[]{1, 2, 3});
}
```

An array destination creates an array whose length equals the number of literal
items. A fixed-size buffer destination keeps its size and pads missing elements;
an unsized buffer destination creates backing storage for its items. A struct
destination uses its declared field types and order, including immutable
parameter fields at the front, and accepts positional or keyed initialization.
Nested literals use their containing element/field types. An inferred declaration
defaults to an array; an empty inferred literal needs an explicit type.

Outside assignments and initializers, a literal must have a type prefix, even
when a function parameter or return type is known: `int[]{1, 2, 3}`,
`buffer<int>{1, 2, 3}`, `buffer<int>[4]{1, 2}`, or `MyStructType{1, 2}`.
Function arguments such as `sum({1, 2})` and bare `return {1, 2};` are rejected.
Explicit literal types are checked against the receiving type. Array and buffer
literals are positional; struct keys must name declared fields. Struct literals are checked by the frontend and emitted as native aggregate values.

Whole-array assignment with `{...}` constructs a new array and rebinds the
variable. It differs from `arr.data = {...}`, which copies into the existing
payload, checks capacity, and pads short literals. That copy behavior also
applies to explicitly typed array/buffer literals on the right of `.data`.
Literal allocations retain the existing stack lifetimes.

Variadic Ocean functions declare their final parameter as `name ...T`. Inside
the function it is an ordinary `T[]` array, with the same inline byte-size header,
`len()`, immutable `.size`, mutable `.data`, data-copy assignments, and checked
indexing. It can be passed directly to another function expecting `T[]`; there
is no separate variadic parameter type or `.length` member.

```ocean
func summarize(values ...int) (int, int) {
    total := 0;
    for (i := 0; i < len(values); i++) {
        total += values[i];
    }
    return total, len(values);
}

func main() {
    total, count := summarize(1, 2, 3);
    total, count = summarize();
    numbers := int[3];
    numbers.data = {4, 5, 6};
    total, count = summarize(numbers...);
    total, count = summarize(10, 20, numbers...);
}
```

Ordinary variadic arguments are collected into a caller-scoped stack array;
zero arguments create an empty array. The final argument of an Ocean variadic
call may use `myArray...` to expand an existing array. Fixed arguments must still
be supplied explicitly. With no additional variadic items, expansion passes the
existing array's values, and the callee gets an independent stack copy. Combining
explicit variadic items with a final expansion builds a fresh array containing
those items followed by the expanded elements.

Variadic parameters can themselves be forwarded with `values...`, since they
are arrays. Expanded expressions are evaluated once. Raw buffer expansion is
rejected because buffers carry no length. C `extern` variadic calls accept a final
array expansion, including runtime-sized arrays and explicit variadic arguments before the expansion. Integer byte and
boolean arguments are promoted to C `int`; floats use C `double`. Fixed arguments
must be supplied explicitly. Raw buffers cannot be expanded.

`...any` accepts mixed scalar arguments and preserves each argument's native type
for forwarding to C, including through other Ocean variadic functions:

```ocean
extern i32 printf(buffer<i8> format, ...);
func println(format string, values ...any) {
    extern.printf((format + "\n\0").data, values...);
}
func main() { println("%d %.1f %s", 7->i32, 2.5, "ok\0".data); }
```

Runtime C calls use libffi. Native builds link it automatically; when linking
saved LLVM IR or library objects using this feature, pass `-lffi` to Clang.
Expanded elements and `any` values currently support numbers, booleans, and
pointers; pass `.data` for strings and arrays at a C boundary. These calls require
at least one fixed C parameter and scalar fixed parameters and results.

Multiple return values unpack into declarations or simultaneous assignments;
`_` discards an individual value. A sole multiple-return call may also supply
all return values (`return summarize(1, 2);`) or all arguments to another call.
Its results are evaluated once. Named results retain type-first declarations,
for example `(int total, count)`, and support bare `return;`.

An executable needs `func main()` or `func main() int`. A generated C ABI entry
point calls Ocean main and uses its result as the process exit status; a void
main exits with zero. Locals without initializers are zero-initialized.
Value-returning functions must explicitly return on every emitted path.

Range loops support arrays and strings: `for (i, item := range arr) { ... }`.
The iterable expression is evaluated once, the index starts at zero, and each
item is copied from `arr[i]` before the body. The index increments after the body,
including on `continue`. A single binding receives only the index. Assigning to
the visible index binding does not affect traversal; it is reset to the next
sequential index before each iteration.

Type declarations distinguish definitions from aliases:

```oc
type cStr buffer<i8>;  // A distinct type with the same representation.
type CString = cStr;  // Another name for exactly the same type.

func first(text cStr) i8 { return text[0]; }
func example(raw buffer<i8>) i8 {
    text := raw->cStr;
    alias: CString = text;
    rawAgain := alias->buffer<i8>;
    return first(rawAgain->cStr);
}
```

A definition `type Name Existing` preserves the underlying storage layout and
operations, but introduces a new identity. Assignments, parameters, results,
struct fields, nested array/buffer element types, and exact generic constraints
require that identity; the underlying type and other definitions with the same
layout are not interchangeable. Use an explicit arrow cast to convert between
layout-equivalent definitions and their underlying type. Numeric definitions
also support the existing explicit numeric casts. For example,
`type UserID int; id := 42->UserID;` keeps `UserID` distinct from `int`.

An alias `type Name = Existing` introduces no identity or conversion: either name
can be used everywhere the other is accepted. Aliases may refer to definitions,
structs (including generic structs), arrays, buffers, interfaces, or other
aliases. Forward references and local declarations work, and cyclic aliases or
inline definitions are rejected. Recursive definitions through pointers are
supported; runtime-sized buffers cannot be used in type declarations.

Defined types can declare their own value or pointer receiver methods, including
numeric and buffer definitions. Methods are not inherited from the underlying
type. Aliases share their target's method set. Boxed `any` values and type-switch
cases preserve defined identity; an alias matches its target. A `~T` constraint
explicitly matches underlying representations, including distinct definitions.

Ungrouped casts accept `value->buffer<T>` as well as `value->Name`. Pointer,
qualified, and other compound targets retain the grouped syntax, such as
`value->(*UserID)` or `value->(pkg.UserID)`.

Unsupported backend features produce an error: calls through interface methods,
enums, other collection types, operator dispatch, closures,
function values, and defer statements.
The parser accepts more syntax than the backend currently implements.

Method receivers use `name type`. Generic receiver arguments bind type parameters
from the struct, so `func (n *Node<T>) set(value T) { n.value = value; }` needs no
`<T any>` after the method name. Receiver bindings inherit struct constraints and
are visible throughout the method signature and body. Method-specific generic
parameters must use different names. Declare them after the method name and
supply them explicitly at the call:

```Ocean
struct Box<T> { value T; }
func (b Box<T>) pair<U>(other U) (T, U) { return b.value, other; }
// In a function:
// b := Box<int>{3};
// number, flag := b.pair<bool>(true);
```

Method type arguments are not inferred. Direct method calls compile to a
specialization for each combination of concrete receiver and method type arguments.
Pointer receivers borrow addressable values automatically; value receivers copy
the struct, including when invoked through a pointer. Recursive calls, variadic
parameters, and multiple results work with receiver-bound types. Bound method
values and operator dispatch remain unsupported.

Generic declarations may omit constraints: `func myFunc<T>(thing T) {}` and
`struct Box<T> { value T; }` both give `T` the `any` constraint. Grouped names
without a constraint, such as `<T, U>`, each default to `any`.

Generic struct names also describe families of types. `Box` accepts every
specialization of that struct, while `Box<int>` accepts only its integer
specialization. These constraints work in function parameters and results,
generic function constraints (`<T Box>` or `<T Box<int>>`), and generic struct
constraints. Matching is directional: a `Box<int>` satisfies `Box`, but a value
known only as `Box` does not satisfy `Box<int>`. Nested arguments can also be
constraints: `Box<Box>` accepts `Box<Box<int>>` and `Box<Box<bool>>`.

Type-guard switch cases use these same constraints. Cases are tested in source
order; put specific cases before family cases when both should be reachable.
`default` runs only if no case matches, regardless of where it appears.

```ocean
struct Box<T> { value T; }
func inspect(box Box) int {
    switch (item := box.(type)) {
    case Box<int>:
        return item.value;
    case Box:
        return 0;
    default:
        return -1;
    }
}
```

A single case gives its binding the case's type constraint. Multiple case types
(`case int, bool:`), union constraints (`case Box<int> | bool:`), and `default`
keep the binding as `any`. The `~` prefix accepted in generic constraints is
also accepted in cases. Family values use
boxed storage with concrete type identity; narrow them to concrete type
arguments before accessing members. Ordinary concrete structs retain their
native layouts. A bare generic name cannot construct a struct literal.
Type arguments do not perform numeric assignment conversions: `Box<i8>` does
not satisfy `Box<int>`.

The built-in `buffer` follows the same family rules: `case buffer:` matches any
buffer, and `case buffer<int>:` restricts its element type while accepting any
length. Both fixed buffers and unsized buffer views match these constraints;
ordinary arrays, pointers, and structs do not. A `buffer<int>` case binding is an
indexable view into the matched value's boxed snapshot; a bare `buffer` binding
must first be narrowed to an element type. Bare `buffer` also works in function
parameters and generic constraints, including nested constraints such as
`Box<buffer>`. Existing concrete forms (`buffer<int>[4]`, `buffer<int>[]`) keep
their storage and calling conventions.

Value-parameter constraints such as `Box<int>(4)` are not supported. Test the
immutable parameter field with an ordinary `if` inside a concrete case instead.

`null` is a reserved pointer-only value. Write `p: *Node<int> = null`, compare
pointers with `== null` or `!= null`, or return/pass `null` where a pointer is
expected. An inferred declaration such as `p := null` requires an explicit
pointer type instead. Structs, numeric types, booleans, strings, and arrays cannot
be null; structs retain their existing layout and zero initialization.

Explicit null is distinct from the zero-address default of an uninitialized
pointer: `p: *int;` gives `p != null`, while `q: *int = null;` gives `q == null`.
Initialize linked-list terminators explicitly to `null`. The backend uses a
reserved pointer marker for null, so it is also distinct from a zero address
returned by C. Dereferencing explicit null traps; a zero-address pointer also
has no valid object to dereference.


## Implicit returns and diagnostics

When a value-returning function reaches its end, it returns the zero value of
its result type and emits a compilation warning. For multiple results, each
result is zero. This also applies to generics, methods, arrays, and named
results; an explicit bare `return;` with named results still returns their
current values. Void functions can reach their end without a warning.

Warnings are reported during `build`, `build --emit-llvm`, `build --check`, and
`run`, and do not prevent successful compilation. Diagnostics include the
filename, line, column, source excerpt, and caret. Warnings are yellow and errors
red when stderr is a terminal. Redirected output is plain text; set
`CLICOLOR_FORCE=1` to force color or `NO_COLOR=1` to disable it.

## Zero values

`zero(Type)` produces the default value of a supported storage type. Its
argument is type syntax, not a value expression:

```c
count := zero(int);                 // 0
flag := zero(bool);                 // false
text := zero(string);              // empty string
items := zero(int[]);               // empty array
values := zero(buffer<int>[4]);     // four zero elements
node := zero(Node<int>);            // recursively defaulted struct fields
pointer := zero(*Node<int>);        // zero-address pointer, distinct from null
```

The result has the requested type, including instantiated generic structs and
nested pointer/array types. Struct value-parameter fields are also zeroed;
`zero` does not invoke a constructor or require a composite initializer.
Fixed buffers and structs recursively default their contents. Empty arrays and
strings use compile-time zero-header templates. When placed in a variable or
parameter, those templates are copied into owned stack storage like other array
values.

Generic functions and methods can use their type parameters directly:

```c
func reset<T any>(value *T) {
    *value = zero(T);
}

func main() {
    value := 42;
    reset<int>(&value);
}
```

Direct generic function calls require explicit type arguments (`<int>` or
`[int]`) and are specialized for each concrete argument list. Generic function
values and type argument inference remain unsupported. `zero(...)`, like
`sizeof(...)`, uses builtin syntax; a bare variable named `zero` is allowed.

`zero` requires exactly one type. Values, `any` itself, and runtime-sized
inline buffers are rejected. Use a fixed buffer or an array instead. Being
able to obtain a zero value does not make an otherwise incomparable type
support equality.


`zero(Type)` and `sizeof(...)` lower to LLVM constants whenever their storage
types are resolved, including inside concrete generic specializations. This
happens before optimization: aggregate zeros contain constant field values and
empty-array header references, and require no runtime computation of the zero
value. Materializing an owned variable/parameter can still require a stack copy.
`sizeof`
uses an LLVM constant expression so LLVM applies the target's exact padding and
alignment without executing size-calculation instructions. Its value operand
remains unevaluated, so `sizeof(++counter)` does not increment `counter`.

A `panic` statement stops execution with a runtime error:

```oc
func requirePositive(n int) {
    if (n <= 0) panic "expected a positive number";
}
```

`panic` accepts a string expression and requires a semicolon. It prints
`Panic: <message>` to standard error, followed by the source file, line, column,
source excerpt, and call trace using the same diagnostics as array bounds errors.
Execution exits with status 1. A panic terminates its control-flow path, so a
value-returning function may panic instead of returning a value.

C APIs that use `void *` can be declared with `*void`. Explicit casts use
`value -> Type`; pointer and other multi-token types need parentheses:

```oc
extern *void malloc(int bytes);
extern void free(*void memory);

func main() {
    raw := extern.malloc(sizeof(int));
    if (raw == zero(*void)) panic "allocation failed";
    p := raw -> (*int);
    *p = 42;
    extern.free(p -> (*void));
}
```

On the current 64-bit host, `int` supplies the native `size_t` width. `*void`
has pointer storage but cannot be dereferenced until cast to a concrete pointer.
Explicit casts support typed pointers, void pointers, raw buffer pointers, and
pointer/integer conversions. Arrays are values; use `.data` to obtain their raw
buffer pointer before casting. Numeric casts also support integers, floats,
and booleans; casting a number to `bool` tests whether it is nonzero.

Typed pointers and raw buffer pointers convert automatically to `*void` in
arguments, assignments, initializers, and returns. For example, `extern.free(p)` works
when `p` is `*int`, and `raw: *void = p;` preserves its address. Comparisons and
ternaries can also combine a typed pointer with `*void`. Conversion back to a
concrete pointer still requires an explicit cast, such as `raw -> (*int)`.
This rule does not convert `**T` to `**void` or change array element types.

Strings support `==` and `!=` by comparing their lengths and complete byte
contents, including embedded NUL bytes. Independently allocated strings with
identical contents compare equal. This also applies to `i8[]`, the underlying
string type; raw pointers still compare addresses.

Labels and `goto` are supported within a function:

```oc
func main() int {
    n := 0;
again: {
    n++;
    if (n < 5) goto again;
}
    goto done;
    return 1;
done: return n;
}
```

Label names are function-local and must be unique. Forward and backward jumps
can target the same block or an enclosing block. A jump cannot enter a nested
block, loop, or another conditional branch, or skip a variable declaration that
would be in scope at the destination. Leaving a scope releases its temporary
stack storage, while retaining array values copied into an enclosing scope.
Use `name: { ... }` for a labeled block; `name: Type` remains declaration syntax.
Labels on loops also support `break name;` and `continue name;` from nested loops.

`return panic` reports a failure at the call site of the current function:

```oc
func requirePositive(n int) int {
    if (n <= 0) return panic "expected a positive number";
    return n;
}

func main() int {
    return requirePositive(-1); // Diagnostic points to this call.
}
```

Like `panic`, it evaluates a string message and terminates execution with status
1. It works in functions with any return signature, including no results. The
source location and excerpt come from the immediate caller; the full call trace
is preserved. In `main`, where there is no Ocean caller, it reports the
`return panic` statement instead. Source excerpts remain embedded in the
executable and do not depend on the source file existing at runtime.

`T[]` is homogeneous: every element has the same concrete type, which must
satisfy `T`. `mixed T[]` permits different concrete types, and each element must
satisfy the interface `T`. `any` is equivalent to the empty `interface{}`.

```oc
interface Value { Get() int; }
struct A { n int; }
struct B { n int; }
func (a A) Get() int { return a.n; }
func (b B) Get() int { return b.n; }

func main() {
    uniform: Value[] = {A{1}, A{2}};
    varied: mixed Value[] = {A{1}, B{2}};
    anything := mixed any[]{1, "two", true};
    varied[0] = B{3}; // B satisfies Value.
    // uniform[0] = B{3}; would violate the shared concrete type.
}
```

As a type constraint, `mixed Value[]` also accepts homogeneous `Value[]` and
concrete `A[]` arrays when `A` satisfies `Value`. This applies to generic type
arguments and type-switch cases; the `mixed` constraint permits different
concrete element types but does not require them.

```oc
func count<T mixed Value[]>(items T) int { return len(items); }
// count<Value[]>(uniform), count<A[]>(concrete), and
// count<mixed Value[]>(varied) all satisfy the constraint.
```

`mixed` is a reserved keyword and modifies an array whose element bound is an
interface. Named interfaces check method names, exact parameter/result types,
variadic signatures, embedded interfaces, and embedded type unions. A method
with a pointer receiver requires a pointer value. Allocate a concrete array
such as `A[n]`, then pass it to a `Value[]` or `mixed Value[]` parameter, or use
an interface-array literal; a nonempty `Value[n]` allocation cannot choose its
concrete element type.

Known invalid literals are rejected during semantic checking. Boxed values,
array conversions, indexed writes, and concatenations check conformance at
runtime. Homogeneous arrays also check concrete element type identity. A mixed
array can be converted to a homogeneous one only when all its values share a
type. Runtime violations terminate execution, like failed type assertions.

A type-switch `case any[]` matches homogeneous array types such as `int[]` and
`string[]`. `case mixed any[]` accepts both homogeneous and mixed array types.
Assertions `.(any[])` and `.(mixed any[])` expose boxed elements for iteration;
interface bounds such as `.(mixed Value[])` also check each element. Calling
methods through an interface value still requires narrowing to a concrete type.

Variadic `...any` packs have type `mixed any[]`, so calls such as
`Println("count", 3)` remain valid. `...Value` packs likewise allow different
types that satisfy `Value`.

## Collections

`types.txt` describes the collection model. The compiler supports sized arrays,
raw buffers, heap-backed dynamic arrays, and immutable strings:

```Ocean
package main;

struct Matrix(height, width int) {
    data int[height][width];
}

func inspect(data buffer<int>, count int) int {
    sized := data->buffer<int>[count];
    array := data->int[count];
    value, ok := array[count]; // Out of bounds returns zero and false.
    return len(sized);
}

func main() {
    raw := buffer<int>[5];
    fixed: int[3] = {1, 2, 3};
    combined := fixed + int[2]{4, 5};
    combined += int[1]{6};

    items := make(int[]);
    items.append(2, 3);
    items.prepend(1);
    items.add(1, 8, 9);
    items.append(items...);
    free(items);
    items = {}; // Re-create a freed dynamic array before using it again.

    pointer := new(int[]);
    *pointer = {1, 2};
    free(*pointer);
    free(pointer);

    matrix := Matrix{2, 3}; // Value parameters initialize the field prefix.
    matrix.data[0][0] = 7;
}
```

Every array begins with an immutable `size` field containing its payload size
in bytes. `T[n]` retains a compile-time length in its type, while `array<T>`
accepts arrays of any length:

```Ocean
func copy(values array<int>) array<int> { return values; }
func create(n int) array<int> {
    values: int[n];
    return values;
}
```

An `array<T>` argument is passed as a pointer to its size header and inline
payload; the callee copies the complete value before running its body. Returned
arrays are copied into independent storage. `len`, indexing, checked indexing,
and range use the byte-size header for runtime-sized arrays. Strings and nested
owned elements are deep-copied and cleaned up on scope exit.

`array<T>` is also a length-independent type constraint; use `array<any>` to
match any array. Dynamic arrays and buffers do not match. Bare `array` is not a
valid type or constraint. The name is reserved for types and remains available
for variables. Construct arrays with `T[n]`, then pass or return them as
`array<T>`; `array<T>{...}`, `new(array<T>)`, and `zero(array<T>)` are rejected.
Existing `T[n]` function signatures remain supported. The size header is included
in `sizeof(T[n])`, including zero-length arrays. Empty structs occupy one padding
byte so their array lengths can also be recovered from the payload byte size.

An array's fixed length participates in type checking, and ordinary indexing
checks its bounds. Two-binding indexing returns an element and a success flag.
Buffers remain unchecked; their lexical length is available to `len`, but is
erased when passed to a function. A sized buffer cast restores lexical length
metadata. Dynamic arrays own a heap payload and expose length and capacity; replacing,
extending, or freeing a dynamic array releases its previous allocation. Dynamic array value
copies are independent, including nested dynamic arrays. Strings use the same heap
dynamic array representation, with read-only bytes and capacity fixed to length.

`values.array()` returns an independent `array<T>` copy of a dynamic array's
current elements, excluding unused capacity. Strings and nested owned values
are deep-copied; pointer elements retain their non-owning addresses. Strings
also support `.array()`, returning an independent, mutable `array<i8>`.

Dynamic arrays support `append(...values)`, `prepend(...values)`, and
`add(index, ...values)`. `remove(index)` removes and returns the indexed element,
shifting later elements left; `pop()` removes and returns the last element.
Both decrease length by one and report a bounds error for an invalid index or
an empty array. Removed strings and nested arrays retain independent owned
storage. These mutation methods are unavailable on strings.

`new(T)` accepts any supported storage type, allocates zeroed heap memory
(including padding), initializes the language value to `zero(T)`, and returns
`*T`. For example, `new(int)` returns a pointer to zero and `new(string)` returns
a pointer to an empty string. Scalars, structs, fixed arrays, dynamic arrays, pointers,
named types, and concrete generic types use the same allocation path. Even
zero-sized types receive distinct allocations. Allocation failure traps;
`new(void)` is rejected because `void` has no storage. Release the allocation
with `free(pointer)` when it is no longer needed.

For compatibility, `T[count]` without a literal body in an expression still
constructs a dynamic array; use `value: T[count]` or `T[count]{...}` to construct an array.
Legacy struct value parameters written with the type first are also accepted.

Parameterized struct array fields currently use indirect internal storage.
Their lengths, initialization, indexing, and value copies work, but their exact
inline memory layout is not yet the layout specified in `types.txt`. The dynamic array
header also uses the compiler's indirect value ABI. Do not rely on either layout
for C interoperability; pass a collection's `.data` buffer explicitly.


Scanner identifiers support Unicode 16.0 identifier characters and leading
underscores; a lone `_` remains the discard token. Exported names use Unicode
uppercase letters. UTF-8 character literals decode to Unicode scalar values,
and `\uXXXX`/`\UXXXXXXXX` escapes encode scalars as UTF-8 in strings. Invalid
UTF-8 and surrogate/out-of-range escapes are rejected. Decimal floating literals
accept signed `e`/`E` exponents. Integer scanning preserves magnitudes through
`UINT64_MAX`, with larger literals rejected. Unsigned integers use unsigned division,
remainder, comparisons, and logical right shifts. Mixed integer operations use
64-bit unsigned arithmetic when either operand is unsigned.
