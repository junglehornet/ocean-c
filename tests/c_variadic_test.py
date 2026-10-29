"""Check runtime-expanded C calls against real C varargs and return ABIs."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(lambda p: str(Path(p).resolve()), sys.argv[1:])


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, (args, result)
    return result


with tempfile.TemporaryDirectory(prefix="ocean c varargs ") as directory:
    root = Path(directory)
    native = root / "native.c"
    native.write_text(r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
static int target = 42;
int16_t narrow_fixed(int16_t x, float f, int count, ...) {
    if (x != -300 || f != 1.25f || count != 2) abort();
    va_list ap; va_start(ap, count);
    if (va_arg(ap, int) != -7 || va_arg(ap, double) != 2.5) abort();
    va_end(ap); return -1234;
}
float single_result(int count, ...) { return 1.25f; }

double mixed_args(int8_t small, double fixed, void *pointer, int count, ...) {
    if (small != -3 || fixed != 1.5 || !pointer || count != 4) abort();
    va_list ap;
    va_start(ap, count);
    int number = va_arg(ap, int);
    int64_t wide = va_arg(ap, int64_t);
    double floating = va_arg(ap, double);
    int *p = va_arg(ap, int *);
    va_end(ap);
    if (number != -7 || wide != INT64_C(5000000000) || floating != 2.5 || *p != 9) abort();
    return 8.5;
}
int8_t byte_result(int count, ...) { return -7; }
int32_t int_result(int count, ...) { return -17; }
int64_t wide_result(int count, ...) { return INT64_C(5000000000); }
bool bool_result(int count, ...) { return true; }
int *pointer_result(int count, ...) { return &target; }
void mutate(int *p, ...) {
    va_list ap;
    va_start(ap, p);
    *p = va_arg(ap, int);
    va_end(ap);
}
''')
    source = root / "main.oc"
    source.write_text(r'''package main;
extern float mixed_args(i8 small, float fixed, *void pointer, i32 count, ...);
extern i8 byte_result(i32 count, ...);
extern i32 int_result(i32 count, ...);
extern int wide_result(i32 count, ...);
extern bool bool_result(i32 count, ...);
extern *int pointer_result(i32 count, ...);
extern void mutate(*int p, ...);
extern i16 narrow_fixed(i16 x, f32 f, i32 count, ...);
extern f32 single_result(i32 count, ...);
func narrowRelay(values ...any) i16 {
    return extern.narrow_fixed((-300)->i16, 1.25->f32, 2->i32, values...);
}
func relay(pointer *int, values ...any) float {
    return extern.mixed_args(-3->i8, 1.5, pointer, 4->i32, values...);
}
func emptyAfter(value *int, calls *int) int[] {
    (*value)++;
    (*calls)++;
    return int[0];
}
func main() int {
    x := 9;
    if (relay(&x, -7->i8, 5000000000, 2.5, &x) != 8.5) return 1;
    if (narrowRelay((-7)->i16, 2.5->f32) != -1234) return 10;
    if (extern.narrow_fixed((-300)->i16, 1.25->f32, 2->i32, (-7)->i16, 2.5->f32) != -1234) return 11;
    empty := int[0];
    if (extern.single_result(0->i32, empty...)->float != 1.25) return 12;
    if (extern.byte_result(0->i32, empty...) != -7) return 2;
    if (extern.int_result(0->i32, empty...) != -17) return 3;
    if (extern.wide_result(0->i32, empty...) != 5000000000) return 4;
    if (!extern.bool_result(0->i32, empty...)) return 5;
    if (*extern.pointer_result(0->i32, empty...) != 42) return 6;
    flags: bool[] = {true};
    extern.mutate(&x, flags...);
    if (x != 1) return 7;
    // Repeated dynamic calls release temporary argument storage.
    for (i := 0; i < 10000; i++) extern.mutate(&x, i->i32, empty...);
    if (x != 9999) return 8;
    calls := 0;
    extern.mutate(&x, x->i32, emptyAfter(&x, &calls)...);
    if (x != 9999 || calls != 1) return 9;
    return 0;
}
''')
    ir, obj, exe = root / "main.ll", root / "native.o", root / "program"
    run(compiler, "build", "--emit-llvm", "-o", str(ir), str(source))
    run(clang, "-c", str(native), "-o", str(obj))
    for optimization in ("-O0", "-O2"):
        run(clang, optimization, "-Wno-override-module", "-x", "ir", str(ir),
            "-x", "none", str(obj), "-lffi", "-o", str(exe))
        run(str(exe))
print("C variadic ABI checks passed")
