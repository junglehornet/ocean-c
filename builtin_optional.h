#ifndef OCEAN_BUILTIN_OPTIONAL_H
#define OCEAN_BUILTIN_OPTIONAL_H

#define OCEAN_BUILTIN_OPTIONAL_NAME "__ocean.builtin.Optional"
#define OCEAN_BUILTIN_OPTIONAL_FILE "<builtin Optional>"
// None is tag zero so zero-initialized optionals represent absence.
static inline const char *oceanBuiltinOptionalSource(void) {
    return "enum Optional<T> { None, Some(T) }\n"
           "func (optional Optional<T>) hasValue() bool {\n"
           "    return optional == Optional<T>.Some;\n"
           "}\n"
           "func (optional Optional<T>) unwrap() T {\n"
           "    if (optional == Optional<T>.Some) return optional.(Optional<T>.Some);\n"
           "    return panic \"Optional.unwrap() called on None\";\n"
           "}\n";
}
#endif
