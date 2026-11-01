"""Array concatenation, append/prepend, value copies, and operand evaluation."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean array addition ") as directory:
    root = Path(directory)
    source, executable, ir = root / "main.oc", root / "program", root / "main.ll"

    def program(text, status=0):
        source.write_text("package main; " + text)
        built = run(compiler, "build", str(root), "-o", str(executable))
        assert built.returncode == 0, built.stdout + built.stderr
        result = run(str(executable))
        assert (result.returncode, result.stdout, result.stderr) == (status, "", ""), result
        emitted = run(compiler, "build", str(root), "--emit-llvm", "-o", str(ir))
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        for optimization in ("-O0", "-O2"):
            built = run(clang, optimization, "-x", "ir", str(ir), "-o", str(executable), "-lffi")
            assert built.returncode == 0, built.stderr
            result = run(str(executable))
            assert (result.returncode, result.stdout, result.stderr) == (status, "", ""), result

    def rejected(text):
        source.write_text("package main; " + text)
        result = run(compiler, "build", str(root), "--check")
        assert result.returncode != 0 and "semantic error:" in result.stderr, result

    program("func main() int { a := int[]{1, 2}; b := int[]{3, 4}; c := a + b; "
            "if (len(c) != 4 || c.size != 32 || c[0] != 1 || c[3] != 4) return 1; "
            "c[0] = 9; b[1] = 8; if (a[0] != 1 || c[3] != 4) return 2; return 0; }")
    program("func main() int { a := int[]{2, 3}; c := 1 + a + 4; "
            "if (len(c) != 4 || c[0] != 1 || c[1] != 2 || c[3] != 4 || len(a) != 2) return 1; "
            "return len(0 + c + int[]{5}) + sizeof(c + 6); }", 14)
    program("func main() int { empty := int[]{}; a := empty + empty; b := empty + 7; "
            "c := 8 + empty; d := empty + int[]{9}; "
            "if (len(a) != 0 || len(b) != 1 || b[0] != 7 || c[0] != 8 || d[0] != 9) return 1; return 0; }")
    program("func main() int { flags := true + bool[]{false} + true; "
            "floats := 1.5 + float[]{2.5} + 3.5; "
            "if (!flags[0] || flags[1] || !flags[2] || floats[2] != 3.5) return 1; "
            "s := (65 -> i8) + \"bc\" + (68 -> i8); "
            "return len(s) + s[0] + s[3]; }", 137)
    program("func main() int { a := int[][]{{1}, {2, 3}}; b := int[][]{{4}}; "
            "c := a + b; d := int[]{0} + a + int[]{5}; "
            "c[0][0] = 9; d[2][0] = 8; b[0][0] = 7; "
            "if (a[0][0] != 1 || a[1][0] != 2 || c[2][0] != 4 || "
            "len(d) != 4 || d[0][0] != 0 || d[3][0] != 5) return 1; return 0; }")
    program("struct Item<T> { value T; items int[]; } "
            "func main() int { x := Item<int>{3, {1}}; a := Item<int>[]{{4, {2}}}; "
            "b := x + a + x; b[0].items[0] = 9; b[1].items[0] = 8; "
            "if (x.items[0] != 1 || a[0].items[0] != 2 || b[2].items[0] != 1) return 1; return 0; }")
    program("func join<T>(a T[], b T[], x T) T[] { return x + a + b + x; } "
            "func main() int { a := int[]{2}; result := join<int>(a, int[]{3}, 1); "
            "s := join<string>(string[]{\"b\"}, string[]{\"c\"}, \"a\"); "
            "result[1] = 9; if (a[0] != 2 || len(result) != 4 || s[1][0] != 98) return 1; "
            "return len(s) + len(s[0]); }", 5)
    program("type P = *int; func main() int { x := 3; p: P = &x; a: P[] = {p}; b := null + a + null; "
            "if (b[0] != null || b[1] != p || b[2] != null) return 1; return *b[1]; }", 3)
    program("func mutate(p *int[], calls *int) int { (*calls)++; (*p)[0] = 9; return 2; } "
            "func main() int { a := int[]{1}; calls := 0; b := a + mutate(&a, &calls); "
            "if (b[0] != 1 || b[1] != 2 || a[0] != 9 || calls != 1) return 1; return 0; }")
    program("func mutate(p *int[]) int[] { (*p)[0] = 9; return int[]{2}; } "
            "func main() int { a := int[]{1}; b := a + mutate(&a); "
            "if (b[0] != 1 || b[1] != 2 || a[0] != 9) return 1; return 0; }")
    program("func main() int { a := int[]{1}; saved := a; a += 2; a += int[]{3}; "
            "if (len(saved) != 1 || saved[0] != 1 || len(a) != 3 || a[2] != 3) return 1; "
            "for (i := 0; i < 10; i++) a += i; return len(a); }", 13)
    program("func main() int { a := any[]{1}; b := a + int[]{2, 3}; c := int[]{4} + a; "
            "d := b + 5; return len(b) + len(c) + len(d) + (b[1].(int)); }", 11)
    program("func main() int { a := buffer<int>[2][]{ {1, 2} }; "
            "b := buffer<int>[2]{3, 4}; c := b + a + b; c[1][0] = 9; "
            "if (a[0][0] != 1 || b[0] != 3) return 1; return len(c); }", 3)
    rejected("func main() { a := int[]{1} + bool[]{true}; }")
    rejected("func main() { a := int[]{1} + false; }")
    rejected("func main() { a := 2.5 + int[]{1}; }")
    rejected("func main() { a := int[]{1} - int[]{2}; }")
    rejected("func main() { a := buffer<int>[1]{1} + buffer<int>[1]{2}; }")
    rejected("func main() { x := 1; x += int[]{2}; }")
    rejected("func main() { a := int[]{1}; a += any[]{true}; }")
    rejected("struct Holder(int[] values) {} func main() { h := Holder{int[]{1}}; h.values += 2; }")

print("array addition passed in native, -O0, and -O2 builds")
