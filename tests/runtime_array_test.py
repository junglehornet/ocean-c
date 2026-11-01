"""Array byte-size headers, length erasure, ABI copies, and constraints."""
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
            built = run([clang, optimization, "-Wno-override-module", "-x", "ir", str(ir), "-lffi", "-o", str(executable)])
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

    module = program('''func mutate(a array<int>) array<int> { a[0] = 99; return a; }
func fixed(a int[3]) int[3] { a[1] = 88; return a; }
func create(n int) array<int> { a: int[n]; if (n > 0) a[0] = 7; return a; }
func main() int {
    a: int[3] = {1, 2, 3}; b := mutate(a); c := fixed(a); d := create(4);
    if (a[0] != 1 || b[0] != 99 || len(b) != 3 || b.size != 24 || a.size != 24) return 1;
    if (c[1] != 88 || a[1] != 2 || len(d) != 4 || d[0] != 7 || d.size != 32) return 2;
    e := create(0); if (len(e) != 0 || e.size != 0) return 3;
    return 0;
}''')
    assert 'mutate"(ptr %arg0)' in module, module
    program('''func copy(a array<i8>) array<i8> { return a; }
func main() int {
    a: i8[3] = {1, 2, 3}; b := copy(a); b[0] = 9;
    if (a.size != 3 || b.size != 3 || len(b) != 3 || a[0] != 1) return 1;
    x, ok := b[3]; if (ok || x != 0) return 2;
    x, ok = b[2]; if (!ok || x != 3) return 3;
    sum := 0; for (i, v := range b) sum += v;
    if (sum != 14) return 4;
    return 0;
}''')
    program('''func concatenate(a array<int>, b array<int>) array<int> { return a + b; }
func main() int {
    a: int[2] = {1, 2}; b: int[1] = {3}; c := concatenate(a, b);
    if (len(c) != 3 || c[0] != 1 || c[2] != 3 || c.size != 24) return 1;
    c += b; if (len(c) != 4 || c[3] != 3) return 2;
    return 0;
}''')
    program('''func count<T array<any>>(a T) int { return len(a); }
func integers<T array<int>>(a T) int { return a[0]; }
func main() int {
    a: int[2] = {8, 9}; b: i8[3] = {1, 2, 3};
    if (count<int[2]>(a) != 2 || count<i8[3]>(b) != 3 || integers<int[2]>(a) != 8) return 1;
    array := 6; return array - 6;
}''')
    program('''func anyArray(a array<any>) int { return len(a); }
func main() int { a: int[2] = {1, 2}; return anyArray(a) - 2; }''')
    program('''struct Grid { cells int[2][3]; }
func copy(a array<int[2]>) array<int[2]> { a[1][0] = 9; return a; }
func main() int {
    g := Grid{{{1, 2}, {3, 4}, {5, 6}}}; a := copy(g.cells);
    if (len(a) != 3 || a.size != 3 * sizeof(g.cells[0]) || a[1][0] != 9 || g.cells[1][0] != 3) return 1;
    p := new(int[2]); (*p)[0] = 5;
    if (p.size != 16 || (*p)[0] != 5) return 2; free(p); return 0;
}''')
    program('''func inspect(value any) int {
        a := value.(array<int>); a[0] = 7;
        switch (b := value.(type)) {
        case array<int>: if (len(b) != 2 || b[0] != 1) return 1;
        default: return 2;
        }
        return 0;
    }
    func main() int { a: int[2] = {1, 2}; return inspect(a); }''')
    program('''array := 3;
    func count(a array<int>) int { return len(a); }
    func main() int { a: int[2]; return count(a) + array - 5; }''')
    program('''func create(n, m int) array<array<int>> {
        grid: int[n][m]; grid[1][1] = 7; return grid;
    }
    func copy(a array<array<int>>) array<array<int>> { a[0][0] = 9; return a; }
    struct Values(count int) { data int[count]; }
    func count(a array<int>) int { return len(a); }
    func main() int {
        grid := create(3, 2);
        if (len(grid) != 2 || len(grid[0]) != 3 || grid[1][1] != 7) return 1;
        fixed: int[2][2] = {{1, 2}, {3, 4}}; changed := copy(fixed);
        if (changed[0][0] != 9 || fixed[0][0] != 1 || changed[1][1] != 4) return 2;
        values := Values{3}; if (count(values.data) != 3 || values.data.size != 24) return 3;
        p := new(int[2]); p.data[0] = 8; if ((*p)[0] != 8) return 4; free(p);
        fixed.data[0][0] = 6; if (fixed[0][0] != 6) return 5;
        return 0;
    }''')
    program('''struct Empty {}
    func count(a array<Empty>) int { return len(a); }
    func create(n int) array<Empty> { a: Empty[n]; return a; }
    func main() int {
        a: Empty[3]; b := create(4);
        if (sizeof(Empty) != 1 || a.size != 3 || b.size != 4 || len(b) != 4 || count(a) != 3) return 1;
        return 0;
    }''')
    program('''func constant() array<int> { a: int[2] = {1, 2}; return a; }
    func pair(a array<int>) (array<int>, int) { return a, len(a); }
    func main() int {
        a := constant(); b, n := pair(a);
        if (len(b) != 2 || n != 2 || b[1] != 2) return 1;
        arrays: array<int>[2] = {a, b}; arrays[0][0] = 8;
        if (a[0] != 1 || arrays[1][0] != 1) return 2;
        return 0;
    }''')
    program('''func copy(values int[]) array<int> { return values.array(); }
    func produce() int[] { return int[]{5, 6}; }
    func main() int {
        source: int[] = {1, 2, 3}; result := source.array();
        if (len(result) != len(source) || result.size != 24 || result[2] != 3) return 1;
        result[0] = 9; source[1] = 8; source.append(4);
        if (source[0] != 1 || result[1] != 2 || len(result) != 3) return 2;
        returned := copy(source); temporary := produce().array();
        if (returned[1] != 8 || len(returned) != 4 || temporary[0] != 5) return 3;
        empty := make(int[]).array(); if (len(empty) != 0 || empty.size != 0) return 4;
        p := new(int[]); *p = {7, 8}; heap := p.array(); free(p);
        if (heap[0] != 7 || heap[1] != 8) return 5;
        text := "abc"; bytes := text.array(); bytes[0] = 'x';
        if (text != "abc" || bytes[0] != 'x' || bytes.size != 3) return 6;
        return 0;
    }''')
    program('''values: int[] = {4, 5}; snapshot := values.array();
    func main() int { values[0] = 8; return snapshot[0] - 4; }''')
    program('''func main() int {
        values: any[] = {1, 2}; a := values.array();
        if (len(a) != 2 || a[0].(int) != 1) return 1;
        mixedValues: mixed any[] = {1, "two"}; b := mixedValues.array();
        if (len(b) != 2 || b[1].(string) != "two") return 2;
        return 0;
    }''')
    rejected('func main() { a: int[] = {1}; a.array(0); }', 'requires no arguments')
    rejected('func main() { a: int[] = {1}; a.array(a...); }', 'requires no arguments')
    program('func main() int { a: int[] = {1}; b := a.array(); return b[1]; }', 1, 'out of bounds')
    for declaration in ('type array int;', 'type array = int;', 'struct array {}', 'interface array {}'):
        rejected(declaration + ' func main() {}', 'reserved type name')
    rejected('func count<T array>(a T) {} func main() {}', 'array requires')
    rejected('func main() { a: array; }', 'array requires')
    rejected('func main() { a: array<int>; }', 'initializer')
    rejected('func main() { a := array<int>{1, 2}; }', 'cannot instantiate')
    rejected('func main() { a := zero(array<int>); }', 'cannot instantiate')
    rejected('func main() { a := new(array<int>); }', 'cannot instantiate')
    rejected('func main() { a: int[2]; a.size = 0; }', 'immutable')
    rejected('func main() { a: int[2]; p := &a.size; }', 'immutable')
    rejected('func take(a array<int>) {} func main() { a: i8[2]; take(a); }', 'type mismatch')
    rejected('func take(a array<int>) {} func main() { a: int[] = {1}; take(a); }', 'type mismatch')
    rejected('func take(a int[2]) {} func relay(a array<int>) { take(a); } func main() {}', 'type mismatch')
    rejected('func integers<T array<int>>(a T) {} func main() { a: i8[2]; integers<i8[2]>(a); }', 'constraint')
    for operation in ('a[-1]', 'a[2]'):
        program('func take(a array<int>) int { return ' + operation + '; } func main() int { a: int[2]; return take(a); }', 1, 'out of bounds')

print(f"{checks} runtime array checks passed")
