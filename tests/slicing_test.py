"""Slice types, copied storage, bounds, and operand evaluation."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean slicing ") as directory:
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

    program("func main() int { a := {0, 1, 2, 3, 4}; b := a[1:3]; "
            "if (len(b) != 2 || b.capacity != 2 || b[0] != 1 || b[1] != 2) return 1; "
            "b[0] = 9; if (a[1] != 1) return 2; "
            "if (len(a[:]) != 5 || len(a[:2]) != 2 || len(a[2:]) != 3 || len(a[5:]) != 0) return 3; "
            "d := int[]{0, 1, 2, 3}; e := d[1:3]; e[0] = 9; "
            "if (d[1] != 1 || e.capacity != 2) return 4; "
            "raw := buffer<int>[4]{0, 1, 2, 3}; f := raw[1:3]; f[0] = 8; "
            "if (raw[1] != 1 || len(raw[:]) != 4) return 5; "
            "s: string = \"abcde\"[1:3]; if (s != \"bc\" || len(s[:0]) != 0) return 6; "
            "return 0; }")
    program("func cut<T>(a T[]) T[] { return a[1:]; } "
            "func main() int { a := int[][]{{1}, {2}, {3}}; b := a[1:2]; b[0][0] = 8; "
            "if (a[1][0] != 2) return 1; c := cut<int>(int[]{1, 2, 3}); "
            "return c[0] + len(c); }", 4)
    program("func tail(a array<int>) int[] { return a[1:]; } "
            "func raw(a buffer<int>[], n int) int[] { return a[:n]; } "
            "func main() int { a := {1, 2, 3}; b := tail(a); "
            "r := buffer<int>[3]{4, 5, 6}; c := raw(r, 2); "
            "if (len(b) != 2 || b[0] != 2 || c[1] != 5) return 1; return 0; }")
    program("func change(a *int[], calls *int) int { (*calls)++; (*a)[1] = 9; return 1; } "
            "func main() int { a := int[]{0, 1, 2}; calls := 0; b := a[change(&a, &calls):]; "
            "if (calls != 1 || b[0] != 1 || a[1] != 9) return 1; return 0; }")
    program("func main() int { a := int[]{0, 1, 2, 3}; b := a[:][1:3]; "
            "f := func() int[] { return a[1:]; }; c := f(); "
            "if (b[0] != 1 || len(c) != 3) return 1; return 0; }")
    program("func main() int { b := {0, 1, 2, 3, 4}[1:3]; "
            "a := mixed any[]{1, true, \"x\"}; c := a[1:]; c[0] = 2; "
            "if (b[0] != 1 || b[1] != 2 || len(c) != 2) return 1; return 0; }")
    program("func main() int { n := 4; a: int[n]; a[2] = 7; b := a[2:]; "
            "raw := buffer<int>[n]; raw[1] = 9; c := raw[1:3]; all := raw[:]; "
            "if (len(b) != 2 || b[0] != 7 || c[0] != 9 || len(all) != 4) return 1; return 0; }")
    for bounds in ("-1:2", "2:1", ":4", "4:", ":-1"):
        source.write_text("package main; func main() { a := int[]{0, 1, 2}; b := a[" + bounds + "]; }")
        built = run(compiler, "build", str(root), "-o", str(executable))
        assert built.returncode == 0, built.stdout + built.stderr
        result = run(str(executable))
        assert result.returncode == 1 and "bounds" in result.stderr.lower(), result
    rejected("func main() { a := 1; p := &a; b := p[:2]; }")
    rejected("func main() { a := 1; b := a[:]; }")
    rejected("func main() { a := int[]{1}; b := a[true:]; }")
    rejected("func main() { a := map[string]int{}; b := a[:]; }")

print("slicing passed in native, -O0, and -O2 builds")
