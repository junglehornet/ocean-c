#ifndef OCEAN_BUILTIN_RESULT_H
#define OCEAN_BUILTIN_RESULT_H

#define OCEAN_BUILTIN_RESULT_NAME "__ocean.builtin.Result"
#define OCEAN_BUILTIN_RESULT_FILE "<builtin Result>"
// Embedded language definitions; these never depend on a standard-library checkout.
static inline const char *oceanBuiltinResultSource(void) {
    return "enum Result<T> { value(T), error(string) }\n"
           "func (result Result<T>) unwrap() T {\n"
           "    if (result == Result<T>.value) return result.(Result<T>.value);\n"
           "    return panic result.(Result<T>.error);\n"
           "}\n"
           "func (result Result<T>) unwrapOrElse(fallback T) T {\n"
           "    if (result == Result<T>.value) return result.(Result<T>.value);\n"
           "    return fallback;\n"
           "}\n"
           "func (result Result<T>) unwrapOrZero() T {\n"
           "    if (result == Result<T>.value) return result.(Result<T>.value);\n"
           "    return zero(T);\n"
           "}\n";
}
#endif
