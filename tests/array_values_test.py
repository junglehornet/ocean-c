"""Runtime-sized owned arrays, indirect by-value calls, and explicit array pointers."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean array values ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("values.oc", "module.ll", "native"))
    checks = 0

    def program(text, expected=0):
        global checks
        source.write_text("package main; " + text)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        module = ir.read_text()
        for optimization in ("-O0", "-O2"):
            built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable)])
            assert built.returncode == 0, built.stdout + built.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout) == (expected, ""), (text, result)
            checks += 1
        return module

    module = program("func change(a int[]) int { a[0] = 99; return a[0]; } "
                     "func main() int { n := 3; a := int[n]; a.data = {1, 2, 3}; "
                     "if (change(a) != 99 || a[0] != 1) return 1; return 0; }")
    callee = re.search(r'define i64 @"ocean.__pkg0_change"\(ptr %arg0\) \{(.*?)\n}', module, re.S).group(1)
    assert 'malloc' in callee and 'getelementptr { i64, i64, ptr }' in callee, callee
    assert "ptr %arg0" in callee, callee

    program("func main() int { a := {1, 2, 3}; b := a; b[1] = 8; "
            "if (a[1] != 2 || b[1] != 8) return 1; b = a; a[0] = 7; "
            "if (b[0] != 1) return 2; return 0; }")
    program("func change(p *int[]) { (*p)[0] = 7; } func main() int { a := {1, 2}; "
            "change(&a); if (a[0] != 7) return 1; return 0; }")
    program("func choose(a int[], p *int[]) int { (*p)[0] = 8; return a[0]; } "
            "func main() int { a := {1, 2}; if (choose(a, &a) != 1 || a[0] != 8) return 1; return 0; }")
    program("func change(p *int[]) int { (*p)[0] = 8; return 0; } "
            "func first(a int[], unused int) int { return a[0]; } "
            "func main() int { a := {1}; if (first(a, change(&a)) != 1 || a[0] != 8) return 1; return 0; }")
    program("func main() int { a := int[][]{{1, 2}, {3}}; b := a; b[0][0] = 9; "
            "if (a[0][0] != 1 || b[0][0] != 9) return 1; return 0; }")
    program("func change(a int[][]) { a[0][0] = 9; a[1] = {7, 8}; } "
            "func main() int { a := int[][]{{1, 2}, {3}}; change(a); "
            "if (a[0][0] != 1 || len(a[1]) != 1) return 1; return 0; }")
    program("struct S { items int[]; } func change(s S) { s.items[0] = 9; } "
            "func main() int { a := S{{1, 2}}; b := a; b.items[0] = 8; change(a); "
            "if (a.items[0] != 1 || b.items[0] != 8) return 1; return 0; }")
    program("struct S { items int[]; } func (s S) change() { s.items[0] = 9; } "
            "func (s *S) share() { s.items[0] = 8; } "
            "func main() int { a := S{{1, 2}}; a.change(); if (a.items[0] != 1) return 1; "
            "a.share(); if (a.items[0] != 8) return 2; return 0; }")
    program("func reset<T>(p *T) { *p = zero(T); } func main() int { a := {1, 2}; "
            "reset<int[]>(&a); if (len(a) != 0) return 1; return 0; }")
    program("func make(n int) int[] { a := int[n]; a.data = {4}; return a; } "
            "func clobber() { b := int[2000]; b.data = {9}; } "
            "func main() int { n := 3; a := make(n); clobber(); "
            "if (len(a) != 3 || a[0] != 4 || a[2] != 0) return 1; return 0; }")
    program("func make(n int) int[][] { a := int[][n]; a[0] = {4, 5}; return a; } "
            "func main() int { a := make(2); if (a[0][1] != 5 || len(a[1]) != 0) return 1; return 0; }")
    program("func main() int { a := {1}; { b := {2, 3}; a = b; b[0] = 9; } "
            "b := int[400]; b.data = {8}; if (len(a) != 2 || a[0] != 2) return 1; return 0; }")
    program("func change(a ...int) { a[0] = 99; } func main() int { a := {1, 2}; "
            "change(a...); if (a[0] != 1) return 1; return 0; }")
    program("func copy<T>(a T[]) T[] { return a; } func main() int { a := {1, 2}; "
            "b := copy<int>(a); b[0] = 9; if (a[0] != 1) return 1; return 0; }")
    program("func main() int { n := 9; a := int[n++]; a.data = {1}; b := a; "
            "if (n != 10 || len(b) != 9) return 1; return 0; }")
    program("func take(a int[]) int { return a[0]; } func main() int { sum := 0; "
            "for (i := 0; i < 80; i++) { a := int[10000]; a.data = {1}; sum += take(a); } "
            "return sum; }", 80)
    program("func main() int { x := 1; a := {&x}; b := a; *b[0] = 9; "
            "if (x != 9 || *a[0] != 9) return 1; return 0; }")

    program("func sum(a int[], n int) int { if (n == 0) return a[0]; a[0]++; return sum(a, n - 1); } "
            "func main() int { a := {1}; if (sum(a, 4) != 5 || a[0] != 1) return 1; return 0; }")
    program("struct S { items int[]; } func (s S) read(unused int) int { return s.items[0]; } "
            "func change(p *S) int { p.items[0] = 8; return 0; } "
            "func main() int { a := S{{1}}; if (a.read(change(&a)) != 1 || a.items[0] != 8) return 1; return 0; }")
    program("func main() int { a := int[][1]; { b: buffer<int[]>[1] = {{1, 2}}; a.data = b; b[0][0] = 9; } "
            "c := int[1000]; c.data = {8}; if (a[0][0] != 1) return 1; return 0; }")
    program("func main() int { a := int[][1]; { a.data = {{1, 2}}; } "
            "c := int[1000]; c.data = {8}; if (a[0][1] != 2) return 1; return 0; }")
    program("func replace(p *int[], source int[]) { *p = source; } "
            "func main() int { a := {1, 2, 3}; b := {7, 8}; replace(&a, b); b[0] = 9; "
            "if (len(a) != 2 || a[0] != 7 || a[1] != 8) return 1; return 0; }")

    program("func main() int { p: *int[] = null; return sizeof(*p); }", 8)

    source.write_text("package main; func replace(p *int[]) { *p = int[]{1, 2}; } "
                      "func main() { a := {0}; replace(&a); }")
    built = run([compiler, "build", str(source), "-o", str(executable)])
    assert built.returncode == 0, built.stdout + built.stderr
    assert run([str(executable)]).returncode == 0, "dynamic array replacement through a pointer must allow growth"

    program("func recur(n int) int[] { if (n == 0) return int[]{1}; a := recur(n - 1); "
            "a[0]++; return a; } func main() int { a := recur(30); return a[0]; }", 31)
    program("func left(n int) int[][] { if (n == 0) return int[][]{{4, 5}, {6}}; return right(n - 1); } "
            "func right(n int) int[][] { return left(n - 1); } "
            "func main() int { a := left(20); b := a; b[0][0] = 9; "
            "if (a[0][0] != 4 || a[0][1] != 5 || a[1][0] != 6) return 1; return 0; }")
    program("struct S { items int[]; } func recur(n int) S { if (n == 0) return S{{3, 4}}; "
            "s := recur(n - 1); s.items[0]++; return s; } "
            "func main() int { s := recur(12); return s.items[0]; }", 15)
    program("func recur<T>(n int, a T[]) T[] { if (n == 0) return a; return recur<T>(n - 1, a); } "
            "func main() int { a := {7, 8}; b := recur<int>(8, a); b[0] = 9; return a[0]; }", 7)

    program("func recur(n int, size int, calls *int) (int[], int) { (*calls)++; "
            "if (n == 0) { a := int[size]; a.data = {6}; return a, 2; } "
            "a, k := recur(n - 1, size, calls); return a, k + 1; } "
            "func main() int { calls := 0; a, k := recur(10, 5, &calls); "
            "if (calls != 11 || len(a) != 5 || a[0] != 6 || a[4] != 0 || k != 12) return 1; return 0; }")
    program("func recur(n int) (int[] result) { if (n == 0) { result = {3}; return; } "
            "result = recur(n - 1); result[0]++; return; } "
            "func main() int { a := recur(9); return a[0]; }", 12)

print(f"{checks} array value checks passed at -O0 and -O2")
