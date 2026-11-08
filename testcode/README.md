# Ocean examples

Each `.oc` file is an independent executable example. Ocean compiles every `.oc`
file in the selected directory as one package, so copy the example you want into
a separate directory before checking or running it. Passing a file inside this
directory still selects all of `testcode/` and causes duplicate declarations.

From the compiler checkout, after building `ocean`:

```sh
example_dir=$(mktemp -d)
cp testcode/arrays.oc "$example_dir/main.oc"
OCEAN_SRC=../ocean-src ./ocean check "$example_dir"
OCEAN_SRC=../ocean-src ./ocean run "$example_dir"
```

Set `OCEAN_SRC` to your standard-library checkout if it is elsewhere. An installed
compiler can use its installed library without this override. `native.oc` and
`strings.oc` call the C `write` function and require a platform that provides it;
the examples were checked and run on Linux.

| Example | Demonstrates |
| --- | --- |
| `helloWorld.oc` | Importing `io` and printing a greeting. |
| `arrays.oc` | Dynamic arrays and pointer parameters that mutate the caller's array. |
| `buffers.oc` | Fixed raw buffer storage and indexed writes. |
| `runtimeBuffers.oc` | Runtime buffer extents and shared raw views. |
| `literals.oc` | Array/buffer literals and function arguments. |
| `functions.oc` | Variadic forwarding and multiple results. |
| `loops.oc` | Counted, condition-only, range, and deferred-condition loops. |
| `statementTest.oc` | Shadowing, branches, and finite examples of loop control. |
| `operatorTest.oc` | Boolean exclusive-or using `!=`, and pointer declarations. |
| `strings.oc` | String ownership, concatenation, mutable byte copies, and C output. |
| `native.oc` | Recursive functions and C output. |
| `structs.oc` | Generic structs, immutable value parameters, and independent copies. |
| `structEtc.oc` | Generic matrices, a named pointer receiver method, enums, and a type alias. |
| `interfaces.oc` | Exact and underlying type lists, numeric operators, and generic method interfaces. |
| `enums.oc` | Variant construction, payload extraction, and exhaustive matching. |

All 15 `.oc` examples pass `ocean check` and run with exit status zero in separate
packages. Loops use deterministic updates and terminate. Formatting uses Ocean
`io`'s `{}` placeholders. Struct operations use named methods; there is no struct
operator overloading or delegated indexing in these examples.

The `.ocexpr` files are expression fragments, without a terminating semicolon,
package declaration, or entry point. Embed them in a function with the appropriate
bindings:

- `simpleMath.ocexpr` expects `factor: int`. With `factor := 2`, its value is `32`.
- `advancedExpressions.ocexpr` expects `number: int`, `offsetPtr: *int`, and
  `isNum: bool`. With `number := 7`, `offsetPtr := &number`, and `isNum := true`,
  its value is `42`. It uses an arrow cast to a pointer type and dereferences
  pointers before arithmetic.

Both fragments pass checking and produce these values in executable wrappers.
