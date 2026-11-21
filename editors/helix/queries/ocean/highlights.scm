; Helix gives later patterns precedence, so general rules come first and more
; specific ones follow. Never capture a node that should keep its own color with
; a `@_name` helper capture: a helper capture also overrides earlier highlights.
; Match keyword and delimiter text directly, as in (keyword "func"), instead.

(identifier) @variable
(keyword) @keyword
(boolean) @constant.builtin.boolean
(null) @constant.builtin
(number) @constant.numeric
(string) @string
(raw_string) @string
(character) @constant.character
(escape_sequence) @constant.character.escape
(comment) @comment
(operator) @operator
(delimiter) @punctuation.delimiter
["(" ")" "[" "]" "{" "}"] @punctuation.bracket

; Capitalized names are types, unless a later rule finds a call or declaration.
((identifier) @type
 (#match? @type "^[A-Z]"))

; Single capital letters are conventionally type parameters.
((identifier) @type.parameter
 (#match? @type.parameter "^[A-Z]$"))

; Lowercase names after a dot are fields. Capitalized ones, such as
; Enum.Variant or pkg.Type, stay types.
(_ (delimiter ".") . (identifier) @variable.other.member
 (#match? @variable.other.member "^[a-z_]"))

((identifier) @type.builtin
 (#match? @type.builtin "^(int|uint|float|bool|string|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|array|buffer|map|void)$"))
(keyword ["any" "mixed"]) @type.builtin
(keyword ["if" "else" "switch" "match" "case" "default" "for" "range" "break" "continue" "fallthrough" "return" "defer" "goto" "panic"]) @keyword.control
(keyword "import") @keyword.control.import
(keyword "func") @keyword.function

; Declarations.
(_ (keyword "package") . (identifier) @namespace)
(_ (keyword ["struct" "enum" "interface" "type"]) . (identifier) @type)

; Keyed fields in a composite literal, such as Options{AltScreen: true}. The
; type before the literal is captured as @type so that it keeps its color.
((identifier) @type . (block "{" . (identifier) @variable.other.member . (delimiter ":"))
 (#match? @type "^[A-Z]"))
((identifier) @type . (block (delimiter ",") . (identifier) @variable.other.member . (delimiter ":"))
 (#match? @type "^[A-Z]"))

; Calls: a name directly followed by an argument list.
(_ (identifier) @function . (parenthesized))
(_ (delimiter ".") . (identifier) @function.method . (parenthesized))

((identifier) @function.builtin
 (#match? @function.builtin "^(len|sizeof|typeof|zero|make|new|free|delete|recover)$"))

; Function and method declarations.
(_ (keyword "func") . (identifier) @function)
; Receiver methods have a parenthesized receiver before the function name.
(_ (keyword "func") . (parenthesized) . (identifier) @function.method)

; Enum variants: a capitalized name after a capitalized type, such as
; Shape.Circle or Shape.Circle(1.0).
((identifier) @type . (delimiter ".") . (identifier) @type.enum.variant
 (#match? @type "^[A-Z]")
 (#match? @type.enum.variant "^[A-Z]"))

; Standard-library package qualifiers, such as io in io.Println.
(_ (identifier) @namespace . (delimiter ".")
 (#match? @namespace "^(io|os|exec|json|http|term|tui|math|mem|time|threads|sync|reflect|testing)$"))
