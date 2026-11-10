#ifndef OCEAN_BUILTIN_OPTIONAL_H
#define OCEAN_BUILTIN_OPTIONAL_H

#define OCEAN_BUILTIN_OPTIONAL_NAME "__ocean.builtin.Optional"
#define OCEAN_BUILTIN_OPTIONAL_FILE "<builtin Optional>"
// None is tag zero so zero-initialized optionals represent absence.
static inline const char *oceanBuiltinOptionalSource(void) {
    return "enum Optional<T> { None, Some(T) }\n";
}
#endif
