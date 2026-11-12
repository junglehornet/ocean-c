#ifndef OCEAN_BUILTIN_OPTIONAL_H
#define OCEAN_BUILTIN_OPTIONAL_H

#define OCEAN_BUILTIN_OPTIONAL_NAME "__ocean.builtin.Optional"
#define OCEAN_BUILTIN_OPTIONAL_FILE "<builtin Optional>"
// none is tag zero so zero-initialized optionals represent absence.
static inline const char *oceanBuiltinOptionalSource(void) {
    return "enum Optional<T> { none, value(T) }\n"
           "func (optional Optional<T>) hasValue() bool {\n"
           "    return optional == Optional<T>.value;\n"
           "}\n"
           "func (optional Optional<T>) unwrap() T {\n"
           "    if (optional == Optional<T>.value) return optional.(Optional<T>.value);\n"
           "    return panic \"Optional.unwrap() called on none\";\n"
           "}\n";
}
#endif
