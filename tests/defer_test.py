"""Deferred call capture, LIFO execution, panic forwarding, and direct recovery."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
PREFIX = '''package main;
extern i32 putchar(i32 c);
func emit(c int) { extern.putchar(c->i32); }
func rescue() { p := recover(); if (p != null) { emit(82); } }
'''


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean defer ") as directory:
    root = Path(directory)
    source, ir, executable = root / "main.oc", root / "main.ll", root / "native"
    checks = 0

    def case(body, output, status=0, panic=None, prefix=PREFIX):
        global checks
        source.write_text(prefix + body)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stderr
        for optimization in ("-O0", "-O2"):
            built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable), "-lffi"])
            assert built.returncode == 0, built.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout) == (status, output), result
            assert (result.stderr.startswith("Panic: " + panic + "\n") if panic is not None else result.stderr == ""), result
            checks += 1

    case('func main() { defer emit(65); defer emit(66); emit(67); }', "CBA")
    case('func f() int { defer emit(65); return 7; } func main() int { return f(); }', "A", 7)
    case('func main() { for (i := 0; i < 4; i++) { defer emit(65+i); } emit(69); }', "EDCBA")
    case('func arg() int { emit(65); return 66; } func main() { x := 67; defer emit(arg()); defer emit(x); x = 68; emit(x); }', "ADCB")
    case('func main() { { defer emit(65); } emit(66); }', "BA")
    case('func f(n int) { defer emit(65+n); if (n > 0) f(n-1); } func main() { f(3); }', "ABCD")
    case('func f() { defer emit(65); panic "direct"; } func main() { defer emit(66); f(); }', "AB", 1, "direct")
    case('func f() { defer emit(65); defer rescue(); defer emit(66); panic "direct"; } func main() { f(); emit(67); }', "BRAC")
    case('func leaf() { panic "leaf"; } func f() { defer rescue(); leaf(); emit(88); } func main() { f(); emit(65); }', "RA")
    case('func f() { defer emit(65); return panic "returned"; } func main() { defer rescue(); f(); }', "R")
    case('func leaf() { return panic "returned"; } func f() { defer emit(65); forward panic leaf(); } func main() { defer rescue(); f(); }', "R")
    case('func leaf() { return panic "returned"; } func f() { defer emit(65); leaf(); } func main() { defer rescue(); f(); }', "AR")
    case('func f() { defer rescue(); forward panic { panic "local"; } } func main() { f(); emit(65); }', "RA")
    case('func leaf() { panic "ordinary"; } func f() { defer rescue(); forward panic leaf(); } func main() { f(); emit(65); }', "RA")
    case('func leaf() { return panic "returned"; } func inner() { defer emit(65); leaf(); } func outer() { defer emit(66); forward panic inner(); } func main() { defer rescue(); outer(); }', "ABR")
    case('func helper() { if (recover() == null) emit(72); } func deferred() { helper(); } func f() { defer rescue(); defer deferred(); panic "active"; } func main() { f(); }', "HR")
    case('func twice() { p := recover(); if (p != null) emit(65); if (recover() == null) emit(66); } func f() { defer twice(); panic "x"; } func main() { f(); }', "AB")
    case('func main() { if (recover() == null) emit(65); defer rescue(); }', "A")
    case('func again() { panic "new"; } func f() { defer rescue(); defer again(); } func main() { f(); emit(65); }', "RA")
    case('func printPanic() { p := recover(); s := p.(string); if (s == "message") emit(65); } func f() { defer printPanic(); panic "message"; } func main() { f(); }', "A")
    case('func mutate(p int*) { *p = 7; } func f() (int n) { defer mutate(&n); return 2; } func main() int { return f(); }', "", 7)
    case('func mutate(p int*) { *p = 8; } func f() (int n) { defer mutate(&n); defer rescue(); panic "x"; } func main() int { return f(); }', "R", 8)
    case('func generic<T>(v T) { emit(v->int); } struct S { c int; } func (s S) show() { emit(s.c); } func main() { s := S{65}; defer s.show(); s.c = 67; defer generic<int>(66); }', "BA")
    case('extern i32 printf(buffer<i8> format, ...); func main() { defer extern.printf("%d\\n\\0".data, 7); }', "7\n")
    case('func output(s string) { if (s == "hello") emit(65); } func main() { s := "hel" + "lo"; defer output(s); s = "other"; }', "A")
    case('func values(xs ...int) { for (i := 0; i < len(xs); i++) emit(xs[i]); } func main() { a: int[] = {65,66}; defer values(a...); a[0] = 67; }', "AB")

    case('func f() { defer rescue(); defer recover(); panic "x"; } func main() { f(); }', "R")
    case('func check(p int*) { emit(*p); } func main() { p := new(int); *p = 65; defer free(p); defer check(p); emit(66); }', "BA")
    case('func main() { xs: int[] = {65,66}; defer free(xs); emit(67); }', "C")
    case('func main() { xs: int[] = {65}; defer xs.append(66); defer emit(67); }', "C")
    case('struct S { c int; } func (s S*) show() { emit(s.c); } func main() { s := S{65}; defer s.show(); s.c = 66; }', "B")
    case('func set(p int*, v int) { *p = v; } func f() (int a, int b) { defer set(&a, 4); defer set(&b, 3); return 1,2; } func main() int { a,b := f(); return a*10+b; }', "", 43)

    case('func bad() int { panic "argument"; } func f() { defer rescue(); defer emit(bad()); } func main() { f(); }', "R")
    case('func empty() { p := recover(); if (p != null && p.(string) == "") emit(65); } func f() { defer empty(); panic ""; } func main() { f(); }', "A")
    case('func again() { panic "new"; } func f() { defer rescue(); defer again(); panic "old"; } func main() { f(); emit(65); }', "RA")
    case('func f() { defer emit(65); switch (1) { case 1: defer emit(66); } if (true) { defer emit(67); } return; } func main() { f(); }', "CBA")

    case('func show(p string*) { if (*p == "kept") emit(65); } func main() { { s := "kept"; defer show(&s); } }', "A")

    case('extern i32 printf(buffer<i8> format, ...); func main() { args: int[] = {7,8}; defer extern.printf("%ld %ld\\n\\0".data, args...); emit(65); }', "A7 8\n")
    case('extern i32 printf(buffer<i8> format, ...); func main() { n: any = 7; defer extern.printf("%ld\\n\\0".data, n); emit(65); }', "A7\n")
    case('func recover() int { return 7; } func main() int { return recover(); }', "", 7, prefix="package main;")

    case('func show(p buffer<i8>) { emit(p[0]->int); } func main() { xs: i8[] = {65}; defer show(xs.data); xs[0] = 66; }', "B")

    case('func innerRescue() { recover(); emit(73); } func deferred() { defer innerRescue(); panic "new"; } func f() { defer rescue(); defer deferred(); panic "old"; } func main() { f(); }', "IR")

    for body in ('func main() { recover(1); }', 'defer missing(); func main() {}', 'func main() { defer 1; }'):
        source.write_text('package main; ' + body)
        result = run([compiler, 'check', str(source)])
        assert result.returncode != 0, result

print(f"{checks} defer and recovery runtime checks passed at -O0 and -O2")
