"""Collection syntax, type identity, bounds, and ownership described in types.txt."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
checks = 0


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean collection types ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("main.oc", "module.ll", "native"))

    def program(text, status=0, diagnostic=None):
        global checks
        source.write_text("package main; " + text)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        for optimization in ("-O0", "-O2"):
            built = run([clang, optimization, "-Wno-override-module", "-x", "ir", str(ir), "-o", str(executable)])
            assert built.returncode == 0, built.stdout + built.stderr
            result = run([str(executable)])
            assert result.returncode == status, (text, result)
            if diagnostic:
                assert diagnostic in result.stderr, result
            else:
                assert result.stdout == result.stderr == "", result
            checks += 1
        return ir.read_text()

    def rejected(text, message):
        global checks
        source.write_text("package main; " + text)
        result = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert result.returncode != 0 and message in result.stderr, result
        checks += 1

    module = program('''
func read(b buffer<int>, n int) int {
    sized := b->buffer<int>[n];
    if (len(sized) != n) return 1;
    a := b->int[n];
    x, ok := a[n];
    if (ok || x != 0) return 2;
    x, ok = a[0];
    if (!ok || x != 7 || len(a) != n) return 3;
    return 0;
}
func main() int {
    b := buffer<int>[5]; b[0] = 7;
    c: buffer<int>[5]; c[0] = 8;
    if (len(c) != 5 || len(b) != 5) return 4;
    return read(b, len(b));
}''')
    assert 'read"(ptr %arg0, i64 %arg1)' in module, module

    program('''func take(a int[3]) int { a[0] = 8; return a[2]; }
func main() int {
    a: int[3] = {1, 2, 3}; b := int[2]{4, 5}; c := a + b;
    if (len(c) != 5 || c[4] != 5 || sizeof(c) != 48) return 1;
    c += b;
    if (len(c) != 7 || c[6] != 5) return 2;
    x, ok := a[3]; if (ok || x != 0) return 3;
    x, ok = a[-1]; if (ok) return 4;
    x, ok = a[1]; if (!ok || x != 2) return 5;
    sum := 0; for (i, value := range a) sum += value;
    if (take(a) != 3 || a[0] != 1 || sum != 6) return 6;
    z: int[0] = {}; if (len(z) != 0 || sizeof(z) != 8) return 7;
    return 0;
}''')
    rejected('func take(a int[3]) {} func main() { a: int[2]; take(a); }', 'type mismatch')
    rejected('func main() { a: int[-1]; }', 'array length must be nonnegative')
    rejected('func read(b buffer<int>) int { return len(b); } func main() {}', 'known length')
    rejected('func read(b buffer<int>[3]) int { return len(b); } func main() {}', 'known length')
    program('func change(b buffer<int>[3]) { b[0] = 7; } func main() int { b: buffer<int>[3]; change(b); return b[0] - 7; }')
    rejected('func main() { n := false; b: int[n]; }', 'type mismatch')
    program('func main() int { a: int[2]; return a[2]; }', 1, 'out of bounds')
    program('func main() int { n := 2; b := buffer<int>[2]; a := b->int[n]; return a[n]; }', 1, 'out of bounds')

    program('''func replace(s *int[]) { *s = {4, 5, 6}; }
func main() int {
    s := make(int[]); s.append(2, 3); s.prepend(1); s.add(3, 4, 5);
    if (len(s) != 5 || s[0] != 1 || s[4] != 5 || s.capacity < len(s)) return 1;
    s.append(s...); if (len(s) != 10 || s[8] != 4) return 2;
    s.add(1, s...); if (len(s) != 20 || s[2] != 2) return 3;
    t := s; t[0] = 9; if (s[0] != 1) return 4;
    replace(&s); if (len(s) != 3 || s[2] != 6) return 5;
    s = {}; if (len(s) != 0) return 6;
    p := new(int[]); if (len(*p) != 0) return 7;
    *p = {8, 9}; (*p).append(10);
    if (len(*p) != 3 || (*p)[2] != 10) return 8;
    free(s); s = make(int[]); s.append(7);
    free(*p); free(p); free(t);
    return s[0] - 7;
}''')
    rejected('func main() { s := make(int[]); free(s); len(s); }', 'has been freed')
    rejected('func main() { s := make(int[]); s.append(true); }', 'type mismatch')
    rejected('func main() { s := make(int[]); s.add(); }', 'requires an index')
    rejected('func main() { s := make(int); }', 'unresolved name')
    program('''func main() int {
    s: int[][] = {{1, 2}, {3}}; t := s;
    s.append(int[]{4, 5}); free(t); s = {{6}};
    if (s[0][0] != 6) return 1; free(s); return 0;
}''')

    program('''struct Matrix(height, width int) { data int[height][width]; }
func area(m Matrix) int {
    if (len(m.data) != m.width || len(m.data[0]) != m.height) return -100;
    m.data[0][0] = 99; return m.height * m.width;
}
func main() int {
    m := Matrix{2, 3}; m.data[2][1] = 7;
    if (area(m) != 6 || m.data[0][0] != 0 || m.data[2][1] != 7) return 1;
    n := Matrix{2, 2, {{1, 2}, {3, 4}}};
    if (n.data[1][1] != 4 || area(n) != 4) return 2;
    return 0;
}''')
    rejected('struct Matrix(height, width int) { data int[height][width]; } '
             'func main() { m := Matrix{2, 3}; m.height = 4; }', 'immutable')
    program('''func mutate(s string) { s = "replacement"; free(s); }
func main() int { s := "hello"; mutate(s);
    if (s != "hello" || len(s) != 5) return 1;
    s += " world"; if (s != "hello world") return 2; return 0;
}''')

    program('''func main() int {
        a: int[] = {10, 20, 30, 40};
        if (a.remove(1) != 20 || len(a) != 3 || a[1] != 30 || a[2] != 40) return 1;
        if (a.pop() != 40 || a.remove(0) != 10 || a.pop() != 30 || len(a) != 0) return 2;
        a.append(7); if (a.pop() != 7 || len(a) != 0) return 3;
        p := new(int[]); *p = {1, 2, 3};
        n := (*p).remove(1); if (n != 2 || (*p).pop() != 3 || (*p)[0] != 1) return 4;
        free(p); return 0;
    }''')
    for body in ('a.remove(-1)', 'a.remove(2)', 'a.pop()'):
        init = '{}' if body == 'a.pop()' else '{1, 2}'
        program('func main() int { a: int[] = ' + init + '; return ' + body + '; }', 1, 'out of bounds')
    for call in ('remove()', 'remove(0, 1)', 'pop(0)', 'remove(int[]{0}...)'):
        rejected('func main() { a: int[] = {1}; a.' + call + '; }', 'requires')
    rejected('func main() { a: int[] = {1}; a.remove("bad"); }', 'type')
    for call in ('remove(0)', 'pop()'):
        rejected('func main() { s := "abc"; s.' + call + '; }', 'immutable')

print(f"{checks} collection type checks passed")
