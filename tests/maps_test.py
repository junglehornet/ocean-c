"""Built-in maps: type syntax, hashing, mixed interfaces, and value ownership."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(p).resolve()) for p in sys.argv[1:])


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=30)


with tempfile.TemporaryDirectory(prefix='ocean maps ') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def program(body, status=0):
        source.write_text('package main;\n' + body)
        built = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert built.returncode == 0, built.stderr
        for optimization in ('-O0', '-O2'):
            built = run(clang, optimization, '-Wno-override-module', '-x', 'ir', str(ir),
                        '-o', str(executable), '-lffi')
            assert built.returncode == 0, built.stderr
            result = run(str(executable))
            assert result.returncode == status, (result, body, optimization)

    def rejected(body, message):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', '--check', str(source))
        assert result.returncode != 0 and message in result.stderr, result

    def traps(body):
        source.write_text('package main;\n' + body)
        built = run(compiler, 'build', str(source), '-o', str(executable))
        assert built.returncode == 0, built.stderr
        assert run(str(executable)).returncode != 0, body

    program('''func main() int {
        m := map[string]int{"one": 1, "two": 2, "one": 4};
        if (len(m) != 2 || m["one"] != 4) return 1;
        m["three"] = 3; m["two"] += 5;
        v, ok := m["missing"];
        if (ok || v != 0 || len(m) != 3) return 2;
        v, ok = m["two"];
        if (!ok || v != 7) return 3;
        delete(m, "one"); delete(m, "absent");
        v, ok = m["one"];
        if (len(m) != 2 || ok || v != 0) return 4;
        total := 0;
        for (k, value := range m) { total += value; if (k == "one") return 5; }
        return total;
    }''', 10)

    program('''func main() int {
        m: map[int]string;
        for (i := 0; i < 160; i++) { m[i * 256] = "value"; }
        for (i := 0; i < 160; i++) { if (m[i * 256] != "value") return 1; }
        for (i := 0; i < 160; i += 2) { delete(m, i * 256); }
        for (i := 1; i < 160; i += 2) { if (m[i * 256] != "value") return 2; }
        if (len(m) != 80) return 3;
        for (i := 0; i < 160; i += 2) { m[i * 256] = "again"; }
        return len(m);
    }''', 160)

    # Two-value reads preserve absence, including empty maps and owned values.
    program('''func main() int {
        empty := map[int]string{};
        value, ok := empty[3];
        if (value != "" || ok || len(empty) != 0) return 1;
        empty[3] = "hi";
        value, ok = empty[3];
        if (value != "hi" || !ok) return 2;
        arrays := map[int]int[]{};
        values, present := arrays[1];
        if (present || len(values) != 0 || len(arrays) != 0) return 3;
        _, present = empty[4];
        if (present) return 4;
        empty[5] = "";
        value, ok = empty[5];
        if (!ok || value != "") return 5;
        boxed := map[any]any{};
        box, boxPresent := boxed["missing"];
        if (boxPresent || box.(int) != 0 || len(boxed) != 0) return 6;
        return 0;
    }''')

    # A missing single-value read is a recoverable Ocean panic, not a trap.
    program('''func lookup() (int result) {
        defer func() {
            message := recover();
            if (message != null && message.(string) == "map key does not exist") result = 7;
        }();
        m := map[int]string{1: "hi", 2: "bye"};
        value := m[3];
        return 99;
    }
    func main() int { return lookup(); }''', 7)

    for body in (
        'func main() { m := map[int]string{1: "hi", 2: "bye"}; value := m[3]; }',
        'func main() { m := map[int]string{}; value := m[3]; }',
        'func main() int { m := map[string]int{}; return m["missing"] + 1; }',
        'func main() { m := map[int]string{}; _ = m[3]; }',
    ):
        source.write_text('package main;\n' + body)
        built = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert built.returncode == 0, built.stderr
        for optimization in ('-O0', '-O2'):
            built = run(clang, optimization, '-Wno-override-module', '-x', 'ir', str(ir),
                        '-o', str(executable), '-lffi')
            assert built.returncode == 0, built.stderr
            result = run(str(executable))
            assert result.returncode == 1, result
            assert 'Panic: map key does not exist' in result.stderr, result
            assert body in result.stderr and 'Call trace' in result.stderr, result

    definitions = '''interface Value { Get() int; }
struct A { n int; } struct B { n int; } struct Missing { n int; }
func (a A) Get() int { return a.n; }
func (b B) Get() int { return b.n; }
'''
    program(definitions + '''func identity(m map[Value]Value) map[Value]Value { return m; }
func main() int {
        m := map[Value]Value{A{1}: B{2}, B{3}: A{4}};
        m[A{1}] = A{5}; m[B{6}] = B{7};
        saved := identity(m); delete(m, B{3});
        if (saved[A{1}].(A).n != 5 || len(saved) != 3 || len(m) != 2) return 1;
        total := 0;
        for (k, v := range saved) {
            switch (x := k.(type)) { case A: total += x.n; case B: total += x.n; }
            switch (x := v.(type)) { case A: total += x.n; case B: total += x.n; }
        }
        return total;
    }''', 26)

    program('''func main() int {
        m := map[any]any{1: "one", "two": 2, true: false};
        m[1.0] = "updated";
        if (len(m) != 3 || m[1].(string) != "updated") return 1;
        if (m[true].(bool) != false || m["two"].(int) != 2) return 2;
        m[int[2]{1, 2}] = 5;
        if (m[int[2]{1, 2}].(int) != 5) return 3;
        delete(m, "two");
        if (len(m) != 3) return 4;
        return 0;
    }''')

    program('''interface Scalar { int | string; }
func main() int { m := map[Scalar]Scalar{1: "one", "two": 2};
        m[3] = "three"; return m["two"].(int) + len(m); }''', 5)

    program('''type Table = map[string]int;
struct Box { data map[string]map[int]string; }
func relay<T>(m map[string]T) map[string]T { return m; }
func main() int {
        empty := make(map[string]int); empty["a"] = 1;
        other := zero(map[string]int); other["a"] = 1;
        if (empty != other) return 1;
        typed: Table = {"a": 2};
        b := Box{map[string]map[int]string{"inner": {1: "old"}}};
        c := b; b.data["inner"][1] = "new";
        if (c.data["inner"][1] != "old") return 2;
        copied := relay<int>(typed); copied["a"] = 3;
        box: any = typed; restored := box.(map[string]int);
        if (restored["a"] != 2 || typed["a"] != 2) return 3;
        arrays := map[string]int[]{"a": {1, 2}};
        n := 1; pointers := map[string]*int{"a": &n};
        return len(arrays["a"]);
    }''', 2)

    program('''func main() int {
        a := map[int]int{0: 1, 8: 2, 16: 3};
        b := map[int]int{16: 3, 8: 2, 0: 1};
        if (a != b) return 1;
        b[100] = 5; delete(b, 100);
        if (a != b) return 2;
        b[0] = 7; if (a == b) return 3;
        return 0;
    }''')

    program('''func main() int {
        m := map[string]int{"a": 1, "b": 2};
        sum := 0;
        for (k, v := range m) { sum += v; delete(m, k); }
        if (len(m) != 0) return 1;
        m["c"] = 9;
        return sum + m["c"];
    }''', 12)

    program('''func main() int {
        m := map[float]int{0.0: 1}; m[-0.0] = 2;
        p := 3; pointers := map[*int]int{&p: 4};
        return len(m) + m[0.0] + pointers[&p];
    }''', 7)

    program('''struct Record { name string; }
func fill(out *map[any]any) {
    for (i := 0; i < 12; i++) { (*out)[i] = Record{"kept"}; }
}
func main() int {
    m := map[any]any{};
    fill(&m);
    for (i := 0; i < 12; i++) {
        if (m[i].(Record).name != "kept") return 1;
    }
    for (i := 0; i < 12; i++) {
        { local := Record{"changed"}; m[i] = local; }
        if (m[i].(Record).name != "changed") return 2;
    }
    copied := m; delete(m, 3);
    if (copied[3].(Record).name != "changed" || len(m) != 11) return 3;
    free(new(map[string]string));
    return len(copied);
}''', 12)

    program('''table: map[string]any = {"initial": "kept"};
func result() map[string]any {
    local := map[string]any{};
    for (i := 0; i < 8; i++) { local["entry"] = "returned"; }
    return local;
}
func size<T map[string]any>(m T) int { return len(m); }
func main() int {
    { table["nested"] = "alive"; }
    if (table["initial"].(string) != "kept" || table["nested"].(string) != "alive") return 1;
    saved := result();
    if (saved["entry"].(string) != "returned") return 2;
    return size<map[string]any>(table);
}''', 2)

    # Qualified interface types inside map syntax survive package rewriting.
    (root / 'ocean.mod').write_text('module example.com/maps\nocean 0.1\n')
    library = root / 'values'; library.mkdir()
    (library / 'values.oc').write_text('package values;\n' + definitions)
    program('''import "example.com/maps/values";
func main() int {
    m := map[values.Value]values.Value{values.A{1}: values.B{2}};
    return m[values.A{1}].(values.B).n;
}''', 2)

    rejected('func main() { m := map[string]int{1: 2}; }', 'type mismatch')
    rejected('func main() { m := map[string]int{"a": true}; }', 'type mismatch')
    rejected('func main() { m := map[string]int{1, 2}; }', 'key: value')
    rejected('func main() { m := map[string]int{}; m[1] = 2; }', 'type mismatch')
    rejected('func main() { m := map[string]int{}; m["a"] = true; }', 'type mismatch')
    rejected('func main() { m := map[string]int{}; p := &m["a"]; }', 'address of a map entry')
    rejected('func main() { m: map[void]int; }', 'void')
    rejected('func main() { m: map[int]void; }', 'void')
    rejected('func main() { m := mixed map[string]any{}; }', 'mixed')
    rejected(definitions + 'func main() { m := map[Value]int{Missing{1}: 2}; }', 'type mismatch')
    rejected(definitions + 'func main() { m := map[int]Value{1: Missing{2}}; }', 'type mismatch')
    rejected('func take(m map[int]string) {} func main() { m := map[string]string{}; take(m); }', 'type mismatch')
    rejected('func main() { m := map[string]int{}; n: map[string]any = m; }', 'type mismatch')
    traps(definitions + 'func main() { x: any = Missing{1}; m := map[Value]int{x: 2}; }')
    traps(definitions + 'func main() { x: any = Missing{1}; m := map[int]Value{1: x}; }')
    traps(definitions + 'func main() { x: any = Missing{1}; m := map[int]Value{}; m[1] = x; }')
    traps(definitions + 'func main() { x: any = Missing{1}; m := map[Value]int{}; m[x] = 2; }')
