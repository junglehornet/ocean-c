# Pending acceptance tests

This inventory covers the incomplete compiler features documented in
[the test README](../README.md). It defines **64 contracts across 21
feature areas**, separate from the regression suites for implemented behavior.
Completed parser contracts are covered by `parser_extensions_test` and the
other parser regression suites.

There are **12 executable probes** of current scanner/printer behavior
and **52 semantic/ownership fixtures** awaiting test drivers. The latter are
acceptance specifications, not claims that missing compiler stages have been
executed or tested successfully. Each fixture has a concrete source and expected
acceptance, diagnostic category, and/or resolved facts.

## Status and execution

Every case is registered individually as `pending.<feature>.<case>` in CTest:

```sh
cmake -S . -B /tmp/ocean-pending -DCMAKE_C_COMPILER=clang -DBUILD_TESTING=ON
cmake --build /tmp/ocean-pending -j2
ctest --test-dir /tmp/ocean-pending -L pending --output-on-failure
ctest --test-dir /tmp/ocean-pending -LE pending --output-on-failure
```

Python 3 is required for tests. Native probes link the public parser, scanner,
and printer APIs. Their checks include parser acceptance, existing AST fields,
scanner values/token kinds, and printer output. Semantic checks intentionally do
not ask `parseTokens` to reject type/label/linkage errors: those belong to later
stages that do not exist yet.

- **XFAIL / exit 77**: checked contract is unmet, or the named compiler-stage
  test driver is absent. CTest displays this as **Skipped**, not Passed.
- **XPASS / exit 1**: the future contract is now satisfied. CTest fails with an
  instruction to promote it into the regular suite and remove its pending entry.
- **ERROR / exit 2**: broken fixture/manifest, missing executable, malformed
  driver response, timeout, unexpected process exit, crash, or sanitizer report.
  These fail rather than being hidden as expected failures.

The one recognized crash is the debug printer's existing explicit
`Unknown expr type` abort for unsupported AST nodes. Other aborts are errors.
Scanner rejection is recognized only for the case's specified diagnostic and
expected exit status. Sanitizer errors always override XFAIL classification.

List contracts or run one with its full explanation:

```sh
python3 tests/pending_features.py --list
python3 tests/pending_features.py --validate
python3 tests/pending_features.py --probe /tmp/ocean-pending/pending_probe \
  --case scientific_numbers.exponent
```

The validator and **26 runner self-tests** are ordinary passing CTest entries.
They check fixture paths, missing expectations, XFAIL/XPASS/error separation,
sanitizer/timeout/crash handling, and adapter responses. The manifest is a
CMake configure dependency, so changing cases refreshes test registration.

## Feature inventory

| Feature | Stage | Cases | Syntax status |
| --- | --- | ---: | --- |
| `unicode` | scanner | 5 | existing |
| `scientific_numbers` | scanner | 1 | proposed |
| `debug_printer` | debug | 6 | proposed |
| `imports` | semantic | 3 | existing |
| `module_discovery` | semantic | 1 | proposed |
| `symbol_binding` | semantic | 3 | existing |
| `type_checking` | semantic | 5 | existing |
| `generic_instantiation` | semantic | 3 | existing |
| `ambiguous_calls` | semantic | 2 | existing |
| `generic_function_values` | semantic | 1 | existing |
| `type_dimension_resolution` | semantic | 3 | existing |
| `constant_evaluation` | semantic | 3 | existing |
| `enum_values` | semantic | 1 | existing |
| `return_checking` | semantic | 4 | existing |
| `label_resolution` | semantic | 5 | existing |
| `extern_linkage` | semantic | 2 | existing |
| `struct_value_application` | semantic | 3 | proposed |
| `operator_resolution` | semantic | 4 | existing |
| `custom_constraints` | semantic | 4 | existing |
| `index_delegation` | semantic | 2 | existing |
| `cleanup` | ownership | 3 | existing |

`existing` means fixtures use currently documented syntax, except for the
specific missing capability. `proposed` means grammar, a spelling, a precedence
choice, or debug output format still needs approval before implementation.
In particular, module marker spelling, struct value application, exponent
notation, and new
printer formats are **proposals, not silently adopted language rules**.
Already implemented syntax and deliberately unsupported forms (e.g. variadic
struct value parameters and generic operator methods) remain in the regression
suite; they are not automatically promised as future features here.

## Compiler-stage driver contract

Once semantic analysis or recursive cleanup exists, configure an executable
adapter rather than changing parser expectations:

```sh
OCEAN_SEMANTIC_TEST_DRIVER=/absolute/path/to/semantic-adapter \
  ctest --test-dir /tmp/ocean-pending -L pending --output-on-failure
OCEAN_OWNERSHIP_TEST_DRIVER=/absolute/path/to/cleanup-adapter \
  ctest --test-dir /tmp/ocean-pending -L pending --output-on-failure
```

A driver is a single executable path (not a shell command). It receives one
JSON request on stdin, runs in a fresh fixture working directory, and writes
one JSON response to stdout. Compiler diagnostics/logs belong on stderr. The
runner creates project files and supplies:

```json
{
  "version": 1,
  "stage": "semantic",
  "feature": "generic_instantiation",
  "source": "...",
  "project_root": "/tmp/ocean-pending-.../project",
  "entry": "main.oc",
  "working_directory": "/tmp/ocean-pending-.../project"
}
```

Other case inputs (e.g. `files` and `extern_symbols`) are forwarded. Expected
answers are **not sent to the driver**. An adapter must query the real compiler
and normalize its results to the fixture facts/diagnostic categories, e.g.:

```json
{"ok": true, "facts": {"x_type": "int"}, "diagnostics": []}
```

```json
{"ok": false, "diagnostics": ["return_type"]}
```

The driver exits zero even for a legitimate compiler rejection; `ok` expresses
acceptance. Nonzero driver exits represent infrastructure failures. Expected
facts are compared individually and expected diagnostic categories must occur
in the response. Extra facts/diagnostics are allowed. Categories and fact names
are adapter-level test conventions, not prescribed compiler APIs or diagnostic
text. Project imports/module layouts and external symbol fixtures likewise
need mapping to the eventual compiler's package/linker interface.

Ownership adapters must exercise actual token/AST destruction on success and
error recovery, using LeakSanitizer or allocation accounting to establish
`live_allocations: 0` and `cleanup_available: true`. Returning an empty parse
result without destruction does not satisfy that contract. Leak detection is
still disabled for the ordinary regression suite until cleanup is implemented.

On promotion, add stronger AST/type/runtime assertions appropriate to the new
implementation, keep malformed-input/recovery tests, and remove the pending
entry. Parser acceptance alone does not establish semantic correctness.
