# Ocean in Helix

`grammar/` is a lightweight Tree-sitter token grammar for highlighting and
balanced-delimiter indentation. The compiler LSP supplies semantic checks and
completion. The grammar supports compiler keywords, Unicode identifiers,
character/string/raw literals, numeric bases and fractions, nested comments,
and Ocean operators. It is not a full syntax parser and does not parse inline
assembly as a separate language.

Copy `grammar/` to `~/.config/helix/grammars/ocean/` and `queries/ocean/` to
`~/.config/helix/runtime/queries/ocean/`. Add the following to `languages.toml`
(using your absolute home path for the grammar source):

```toml
[language-server.ocean-lsp]
command = "ocean"
args = ["check", "--lsp"]

[[language]]
name = "ocean"
scope = "source.ocean"
file-types = ["oc"]
roots = ["ocean.mod", ".git"]
comment-token = "//"
block-comment-tokens = { start = "/*", end = "*/" }
language-servers = ["ocean-lsp"]
grammar = "ocean"
indent = { tab-width = 4, unit = "    " }

[[grammar]]
name = "ocean"
source = { path = "/absolute/home/.config/helix/grammars/ocean" }
```

To build only Ocean, temporarily add `use-grammars = { only = ["ocean"] }`
at the top of languages.toml, run `helix --grammar build`, then remove that
filter if you maintain other grammars. Check with `helix --health ocean`. Restart the
language server with `:lsp-restart` after updating the compiler, and reopen
Helix after updating its grammar. For manual completion, set this in config.toml:

```toml
[keys.insert]
C-space = "completion"
```

The generated ABI 14 parser is included; installation needs a C compiler, but
not the Tree-sitter CLI. After changing grammar.json, regenerate using CLI
0.25.10 from the grammar directory:

```sh
tree-sitter generate --abi 14 grammar.json
cp grammar.json src/grammar.json
tree-sitter test
```

Highlight queries can be checked with `tree-sitter query
../queries/ocean/highlights.scm example.oc`. Identifier ranges mirror unicode.h;
update them alongside changes to the compiler's Unicode tables.
