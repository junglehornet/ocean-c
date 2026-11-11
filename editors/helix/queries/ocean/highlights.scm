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

((identifier) @type.builtin
 (#match? @type.builtin "^(int|uint|float|bool|string|i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|array|buffer|map|void)$"))
((keyword) @type.builtin
 (#match? @type.builtin "^(any|mixed)$"))
((keyword) @keyword.control
 (#match? @keyword.control "^(if|else|switch|match|case|default|for|range|break|continue|fallthrough|return|defer|goto|panic)$"))
((keyword) @keyword.control.import
 (#eq? @keyword.control.import "import"))
((keyword) @keyword.function
 (#eq? @keyword.function "func"))

(_ (keyword) @_keyword . (identifier) @namespace
 (#eq? @_keyword "package"))
(_ (keyword) @_keyword . (identifier) @type
 (#match? @_keyword "^(struct|enum|interface|type)$"))
(_ (keyword) @_keyword . (identifier) @function
 (#eq? @_keyword "func"))
(_ (identifier) @function . (parenthesized))

((identifier) @function.builtin
 (#match? @function.builtin "^(len|sizeof|typeof|zero|make|new|free|delete|recover)$"))

; Receiver methods have a parenthesized receiver before the function name.
(_ (keyword) @_func . (parenthesized) . (identifier) @function.method
 (#eq? @_func "func"))
