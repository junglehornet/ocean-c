"""Check generic-family constraints and native type-switch behavior."""
import pathlib
import subprocess
import sys
import tempfile

compiler, clang = map(str, map(pathlib.Path, sys.argv[1:]))
compiler = str(pathlib.Path(compiler).resolve())
clang = str(pathlib.Path(clang).resolve())

with tempfile.TemporaryDirectory(prefix="ocean constraints ") as directory:
    root = pathlib.Path(directory)
    source, executable, ir = root / "main.oc", root / "main", root / "main.ll"

    def invoke(*args):
        return subprocess.run(args, capture_output=True, text=True, timeout=15)

    def program(body, status):
        source.write_text("package main; " + body)
        built = invoke(compiler, "build", str(root), "-o", str(executable))
        assert built.returncode == 0, built.stdout + built.stderr
        assert invoke(str(executable)).returncode == status
        emitted = invoke(compiler, "build", str(root), "--emit-llvm", "-o", str(ir))
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        optimized = invoke(clang, "-O2", "-x", "ir", str(ir), "-o", str(executable), "-lffi")
        assert optimized.returncode == 0, optimized.stderr
        assert invoke(str(executable)).returncode == status

    def rejected(body, message):
        source.write_text("package main; " + body)
        result = invoke(compiler, "build", str(root), "--check")
        assert result.returncode != 0, body
        assert message in result.stdout + result.stderr, result.stdout + result.stderr

    # Built-in buffers follow the same bare/specialized constraint rules as structs.
    program("func classify(x any) int { switch (x.(type)) { case buffer: return 2; default: return 1; } } "
            "func main() int { a := buffer<int>[2]{1, 2}; b := buffer<bool>[1]{true}; "
            "view: buffer<int>[] = a; n := 3; runtime: buffer<int>[n]; "
            "return classify(a) + classify(b) + classify(view) + classify(runtime) + classify(int[]{1}) + classify(3); }", 10)
    program("func read(x buffer) int { switch (item := x.(type)) { "
            "case buffer<int>: return item[0]; case buffer<i8>: return item[0] -> int; "
            "case buffer: return 2; default: return 99; } } "
            "func main() int { small := buffer<int>[1]{3}; big := buffer<int>[2]{4, 5}; "
            "byte := buffer<i8>[1]{6}; flags := buffer<bool>[1]{true}; view: buffer<int>[] = big; "
            "return read(small) + read(big) + read(byte) + read(flags) + read(view); }", 19)
    program("func read(x any) int { switch (item := x.(type)) { "
            "case buffer<int>[2]: return item[1]; case buffer<int>: return item[0]; default: return 9; } } "
            "func main() int { return read(buffer<int>[2]{1, 3}) + read(buffer<int>[1]{4}); }", 7)
    program("func read<T buffer<int>>(x T) int { return x[0]; } "
            "func isBuffer<T buffer>(x T) int { switch (x.(type)) { case buffer: return 2; default: return 9; } } "
            "func main() int { return read<buffer<int>[1]>(buffer<int>[1]{4}) + "
            "isBuffer<buffer<bool>[2]>(buffer<bool>[2]{true, false}); }", 6)
    program("struct Box<T> { value T; } func classify(x any) int { "
            "switch (x.(type)) { case Box<buffer>: return 3; default: return 1; } } "
            "func main() int { return classify(Box<buffer<int>[1]>{{4}}) + classify(Box<int>{4}); }", 4)
    program("func read(x buffer) int { switch (y := x.(type)) { case buffer: "
            "switch (z := y.(type)) { case ~buffer<int>: z[0] = 7; return z[0]; default: return 1; } } return 2; } "
            "func main() int { x := buffer<int>[1]{4}; result := read(x); return result + x[0]; }", 11)
    program("func classify(x any) int { switch (x.(type)) { case buffer[]: return 3; default: return 1; } } "
            "func main() int { a := buffer<int>[2][]{{1, 2}}; return classify(a) + classify(int[]{1}); }", 4)
    rejected("func f<T buffer>(x T) {} func main() { f<int>(1); }", "constraint")
    rejected("func f<T buffer<int>>(x T) {} func main() { f<buffer<bool>[1]>(buffer<bool>[1]{true}); }", "constraint")
    rejected("func f(x buffer) int { return x[0]; }", "narrow buffer")
    rejected("func main() { x := zero(buffer); }", "concrete buffer element type")
    rejected("func f(x buffer<>) {}", "Expected type argument")

    box = "struct Box<T> { value T; } "
    program(box + "func classify(x Box) int { switch (item := x.(type)) { "
            "case Box<int>: return item.value; case Box: return 2; default: return 3; } } "
            "func main() int { return classify(Box<int>{7}) + classify(Box<bool>{true}); }", 9)
    program("struct Sized<T>(int size) { value T; } func inspect(x Sized) int { "
            "switch (item := x.(type)) { case Sized<int>: return item.size; default: return 1; } } "
            "func main() int { return inspect(Sized<int>{4, 2}) + inspect(Sized<int>{7, 3}); }", 11)
    # Family matching is nominal and the first match wins, even over a later exact case.
    program(box + "struct Other<T> { value T; } func classify(x any) int { "
            "switch (x.(type)) { default: return 5; case Box: return 2; case Box<int>: return 9; } } "
            "func main() int { return classify(Box<int>{1}) + classify(Other<int>{1}); }", 7)
    program(box + "func classify(x any) int { switch (item := x.(type)) { "
            "case Box<Box>: return 3; case Box<int>: return item.value; default: return 9; } } "
            "func main() int { return classify(Box<Box<bool>>{{true}}) + classify(Box<int>{4}); }", 7)
    program(box + "struct Other<T> { value T; } func classify(x any) int { "
            "switch (x.(type)) { case *Box: return 3; case Box[]: return 4; default: return 1; } } "
            "func main() int { a := Box<int>{2}; b := Other<int>{2}; "
            "xs := Box<bool>[]{{true}}; return classify(&a) + classify(&b) + classify(xs); }", 8)
    program("func classify(x any) int { switch (item := x.(type)) { case bool: "
            "if (item) return 3; return 1; case i8: return item -> int; case int: return item; "
            "default: return 9; } } func main() int { return classify(true) + classify(4->i8) + classify(5); }", 12)
    program("func classify(x any) int { switch (x.(type)) { case int, bool: return 3; "
            "case any: return 5; } return 9; } func main() int { return classify(1) + classify(2.5); }", 8)
    program(box + "func exact<T Box<int>>(x T) int { return x.value; } "
            "func family<T Box>(x T) int { switch (item := x.(type)) { "
            "case Box<int>: return item.value; default: return 2; } } "
            "func main() int { return exact<Box<int>>(Box<int>{4}) + family<Box<bool>>(Box<bool>{true}); }", 6)
    program(box + "struct Wrapper<T Box> { value T; } func main() int { "
            "w := Wrapper<Box<int>>{{4}}; return w.value.value; }", 4)
    program(box + "func erase(x Box) Box { return x; } func main() int { "
            "x: Box = erase(Box<int>{6}); switch (item := x.(type)) { "
            "case Box<int>: return item.value; default: return 1; } }", 6)
    program(box + "func make<T>(value T) Box<T> { return Box<T>{value}; } "
            "func get<T>(box Box<T>) T { return box.value; } "
            "func main() int { return get<int>(make<int>(7)); }", 7)
    # Guard and initializer execute once; break, continue, and nested switch scopes work.
    program("func next(n *int) int { (*n)++; return *n; } func main() int { "
            "count := 0; total := 0; for (i := 0; i < 3; i++) { "
            "switch (saved := next(&count); item := next(&count).(type)) { "
            "case int: if (i == 0) continue; total += saved + item; break; default: return 99; } "
            "total++; } return count + total; }", 26)
    program("func main() int { x := 1; switch (x.(type)) {} return 4; }", 4)
    program("func main() int { switch (x := true.(type)) { case bool: "
            "switch (x.(type)) { case bool: break; } return 3; default: return 1; } }", 3)
    program("func main() int { x := 1; done: switch (x.(type)) { case int: break done; "
            "default: return 9; } return 4; }", 4)
    program(box + "func classify(x any) int { switch (x.(type)) { "
            "case Box<int> | ~bool: return 3; case Box: return 2; default: return 1; } } "
            "func main() int { return classify(true) + classify(Box<int>{1}) + classify(Box<bool>{true}); }", 8)
    program("func all(x any) int { switch (x.(type)) { case any: return 7; } } "
            "func main() int { return all(true); }", 7)
    rejected(box + "func use(x Box<int>) {} func main() { use(Box<bool>{true}); }", "type mismatch")
    rejected(box + "func use(x Box<int>) {} func main() { use(Box<i8>{1}); }", "type mismatch")
    rejected(box + "func use<T Box<int>>(x T) {} func main() { use<Box<bool>>(Box<bool>{true}); }", "constraint")
    rejected(box + "func use<T Box>(x T) {} func main() { use<int>(1); }", "constraint")
    rejected(box + "struct Wrapper<T Box<int>> { value T; } func main() { w: Wrapper<Box<bool>>; }", "constraint")
    rejected(box + "func use(x Box) int { return x.value; }", "narrow")
    rejected("func main() { x := 1; switch (x.(type)) { case Missing: break; } }", "unresolved type")
    rejected(box + "func main() { x := Box<int>{1}; switch (x.(type)) { case Box<int>(4): break; } }", "Expected function signature type")

    # Contextual conversion accepts literal syntax, never arbitrary int values.
    program("""
        func identity(x float) float { return x; }
        func result() float { return -3; }
        func main() int {
            x: float = 0;
            x = 2;
            x += 1;
            if (x != 3 || 3 != x || x < 0 || 4 <= x) { return 1; }
            if (identity(2) != 2.0 || identity(-2) != -2.0) { return 2; }
            if (result() != -3.0 || x + 2 != 5.0 || 2 + x != 5.0) { return 3; }
            y: f32 = 2;
            if (y != 2) { return 4; }
            return 0;
        }
    """, 0)
    rejected("func main() { n := 0; x: float = n; }", "type mismatch")
    rejected("func f(x float) {} func main() { n := 0; f(n); }", "type mismatch")
    rejected("func main() { x: float = 0.0; n := 0; if (x == n) {} }", "type mismatch")
    rejected("func f(x float) {} func main() { f(1 + 2); }", "type mismatch")
