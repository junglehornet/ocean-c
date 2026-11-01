"""Labels, forward/backward goto, scope cleanup, and function-local label names."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=15)


with tempfile.TemporaryDirectory(prefix="ocean goto ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("jump.oc", "jump.ll", "native"))
    checks = 0

    def program(text, expected):
        global checks
        source.write_text("package main; " + text)
        built = run([compiler, "build", str(source), "-o", str(executable)])
        assert built.returncode == 0, built.stdout + built.stderr
        result = run([str(executable)])
        assert result.returncode == expected and not result.stderr, (text, result)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        for optimization in ("-O0", "-O2"):
            built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable)])
            assert built.returncode == 0, built.stderr
            result = run([str(executable)])
            assert result.returncode == expected and not result.stderr, (text, result)
            checks += 1

    program('func main() int { goto done; return 1; done: return 7; }', 7)
    program('func main() int { n := 0; again: { n++; if (n < 8) goto again; } return n; }', 8)
    program('func main() int { n := 3; { a := int[200]; a.data = {4}; n += a[0]; goto done; } '
            'return 1; done: return n; }', 7)
    program('func main() int { n := 0; again: { a := int[100000]; a.data = {1}; n += a[0]; '
            'if (n < 100) goto again; } return n; }', 100)
    program('func main() int { n := 0; for (i := 0; i < 9; i++) { '
            'if (i == 3) goto done; n++; } done: return n; }', 3)
    program('func main() int { n := 0; { goto inside; n = 99; inside: { n = 4; } } return n; }', 4)
    program('func main() int { a := int[]{1}; { b := int[]{7, 8}; a = b; goto done; } '
            'done: return a[0] + len(a); }', 9)
    program('func main() int { goto first; first: second: return 6; }', 6)
    program('func f() int { done: return 3; } func main() int { goto done; done: return f(); }', 3)
    program('func make(n int) int[] { goto done; done: return int[]{n}; } '
            'func main() int { a := make(3); b := make(4); return a[0] + b[0]; }', 7)
    program('func make(n int) int[] { if (n > 0) return make(n - 1); goto done; '
            'done: return int[]{7}; } func main() int { a := make(4); return a[0]; }', 7)
    program('func identity<T>(v T) T { goto done; done: return v; } '
            'func main() int { return identity<int>(7); }', 7)
    program('struct S { value int; } func (s S) get() int { goto done; done: return s.value; } '
            'func main() int { s := S{7}; return s.get(); }', 7)
    program('func main() int { value := 7; value: return value; }', 7)
    program('func main() int { return 7; x := 9; }', 7)

    program('func main() int { n := 0; outer: for (i := 0; i < 8; i++) { '
            'for (j := 0; j < 9; j++) { a := int[200]; n++; if (n == 4) break outer; } } return n; }', 4)
    program('func main() int { n := 0; outer: for (i := 0; i < 5; i++) { '
            'for (j := 0; j < 9; j++) { n++; continue outer; } } return n; }', 5)

    for text, expected in [
        ('func main() { goto missing; }', 'undefined label'),
        ('func main() { here: {} for (;;) { break here; } }', 'enclosing loop'),
        ('func main() { again: {} again: {} }', 'duplicate label'),
        ('func main() { goto done; x := 1; done: {} }', 'variable declaration'),
        ('func main() { goto inside; { inside: {} } }', 'cannot enter'),
        ('func main() { goto inside; if (true) inside: {} }', 'cannot enter'),
        ('func main() { if (true) { goto other; } else { other: {} } }', 'cannot enter'),
        ('func main() { goto inside; for (;;) { inside: {} } }', 'cannot enter'),
        ('func f() { here: {} } func main() { goto here; }', 'undefined label'),
    ]:
        source.write_text("package main; " + text)
        result = run([compiler, "build", str(source), '-o', str(executable)])
        assert result.returncode > 0 and expected in result.stdout + result.stderr, result

print(f'{checks} goto runtime checks passed at -O0 and -O2; invalid jumps rejected')
