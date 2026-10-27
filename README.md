# Ocean compiler

Build with CMake and a C23 compiler:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Compile one source file into a native executable (requires `clang` on `PATH`
and the host system's linker and C runtime):

```sh
build/ocean-c testcode/native.oc -o hello
./hello
```

The compiler scans, parses, checks semantics, emits textual LLVM IR, then invokes
Clang to compile and link that IR for the current host. No LLVM development
libraries are needed. Without `-o`, the executable is written to `a.out`.
Compiler and linker failures return a nonzero status and preserve any previous
output file.

To save LLVM IR instead, or only run the frontend:

```sh
build/ocean-c --emit-llvm testcode/native.oc -o hello.ll
clang -x ir hello.ll -o hello
build/ocean-c --check testcode/native.oc
```

`--emit-llvm` defaults to `a.ll` and does not invoke Clang. `--check` retains the
frontend's token dump and can check several files independently; executable
compilation accepts exactly one file.

The initial backend supports:

- `int`/`i64` (signed 64-bit), `i32`, `i8`, `float`/`f64` (double precision), `bool`,
  and `string` (an alias for `i8[]`).
- Functions, recursion, forward calls, typed variadic parameters, multiple return
  values, named results, local variables, and lexical shadowing.
- Arithmetic, comparisons, integer bit operations, assignments, increments,
  numeric casts, ternaries, and short-circuit `&&` and `||`.
- `if`/`else`, ordinary and deferred-condition `for` loops, and unlabeled
  `break` and `continue`.
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

Extern functions use C-style declaration ordering with Ocean type names:

```ocean
extern int write(i32 fd, buffer<i8>[] data, int count);
extern void srand(i32 seed);
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

`string` is exactly `i8[]`: an immutable byte-size header followed by mutable
inline signed bytes. `len(text)` and `text.size` both give the byte count.
String literals contain their decoded bytes with no appended zero byte; embedded
zero bytes are ordinary data. Indexing uses the same bounds checks as arrays,
while `text.data` exposes an unchecked `buffer<i8>[]` view.

```ocean
extern int write(i32 fd, buffer<i8>[] data, int count);

func main() {
    text: string = "Hello" + ", Ocean!";
    bytes: i8[] = text;             // Same array reference
    text += "\n";                    // Fresh concatenated array
    write(1, text.data, len(text));
}
```

`+` concatenates strings (including values declared as `i8[]`) into fresh stack
storage, copying both operands without changing them. `+=` assigns that new
array reference to the variable. String literals and concatenations have the
same block lifetime as other stack arrays; references do not extend that
lifetime. Do not return a locally allocated string or retain it after its block
exits. Empty strings have size zero. C APIs that require a terminator need one
explicitly, for example `"text\0".data`; length-based APIs such as `write`
accept `.data` and `len(text)` directly.

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
`empty: int[];` create an empty header with `len(empty) == 0`. Variables,
assignments, arguments, and returns carry references to the same block and do
not extend its lifetime. Nested arrays store references to their child arrays.
Arrays and buffers are distinct: `len()` accepts arrays, while buffers still
require explicit counts. Arrays support argument expansion as described below; buffers do not.

`sizeof(T)` returns the storage size in bytes of a supported type;
`sizeof(value)` inspects its type without evaluating the value expression.
An array reference occupies pointer-sized storage, while `.size` records the
bytes in the referenced object's payload.

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
are inline fields, while arrays and strings retain their reference semantics.
Positional and keyed literals support nested composites and empty defaults.
Structs can be passed and returned by value, including variadic arguments and
multiple returns. Ordinary field writes, nested indexing, `&field`, dereferencing,
and pointer field access operate on the original storage.

Recursive types must use pointers or array references to break cycles; recursive
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
existing array by reference, so the callee can modify its elements. Combining
explicit variadic items with a final expansion builds a fresh array containing
those items followed by the expanded elements.

Variadic parameters can themselves be forwarded with `values...`, since they
are arrays. Expanded expressions are evaluated once. Raw buffer expansion is
rejected because buffers carry no length. C `extern` variadic calls keep their
native ABI and do not accept array expansion.

Multiple return values unpack into declarations or simultaneous assignments;
`_` discards an individual value. A sole multiple-return call may also supply
all return values (`return summarize(1, 2);`) or all arguments to another call.
Its results are evaluated once. Named results retain type-first declarations,
for example `(int total, count)`, and support bare `return;`.

An executable needs `func main()` or `func main() int`. A generated C ABI entry
point calls Ocean main and uses its result as the process exit status; a void
main exits with zero. Locals without initializers are zero-initialized.
Value-returning functions must explicitly return on every emitted path.

Unsupported backend features produce an error: imports, multiple files,
globals, other collection types, generic functions, operator dispatch,
closures, function values, switches, labels, and defer statements.
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
