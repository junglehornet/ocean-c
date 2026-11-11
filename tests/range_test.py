"""Compile range loops and verify their behavior in native code and LLVM IR."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(os.path.abspath, sys.argv[1:])

def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=15, **kwargs)

with tempfile.TemporaryDirectory(prefix="ocean backend ") as directory:
    root = Path(directory)
    source = root / "source file.oc"
    exe = root / "native executable"
    ir = root / "module.ll"

    def program(text, status=0, stdout=""):
        source.write_text("package main; " + text)
        built = run([compiler, "build", str(source), "-o", str(exe)])
        assert built.returncode == 0, built.stdout + built.stderr
        result = run([str(exe)])
        assert (result.returncode, result.stdout) == (status, stdout), result
        emitted = run([compiler, "build", "--emit-llvm", "-o", str(ir), str(source)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        assert "define i32 @main()" in ir.read_text()
        for optimization in ("-O0", "-O2"):
            verified = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(exe), "-lffi"])
            assert verified.returncode == 0, verified.stderr
            result = run([str(exe)])
            assert (result.returncode, result.stdout) == (status, stdout), result

    # Range loads the current item before the body; continue advances the index.
    program("func main() int { a := int[]{3, 5, 7}; sum := 0; "
            "for (i, item := range a) { sum += i + item; } return sum; }", 18)
    program("func main() int { a := int[]{3, 5, 7}; sum := 0; "
            "for (i := range a) { if (i == 1) continue; sum += a[i]; } return sum; }", 10)
    program("func main() int { sum := 0; for (i, item := range zero(int[])) { sum++; } "
            "for (i, item := range int[]{4, 8}) { sum += item; break; } return sum; }", 4)
    program('func main() int { sum := 0; for (i, ch := range "AB") { sum += ch; } return sum; }', 131)
    program("func main() int { i := 20; item := 30; sum := 0; "
            "outer: for (i, item := range int[]{2, 3}) { "
            "for (j, n := range int[]{4, 5}) { sum += item * n; continue outer; } } "
            "return sum + i + item; }", 70)
    program('func main() int { a := string[]{"ab", "c"}; sum := 0; '
            'for (i, item := range a) { sum += len(item); item = "replacement"; free(item); } '
            'if (a[0] != "ab" || a[1] != "c") return 1; return sum; }', 3)
    program("func make(n *int) int[] { (*n)++; return int[]{4, 6}; } "
            "func main() int { n := 0; sum := 0; for (i, item := range make(&n)) { "
            "sum += item; } return sum + n; }", 11)
    program("func main() int { a := int[]{1, 2, 3}; sum := 0; "
            "for (i, item := range a) { if (i == 0) a[1] = 9; sum += item; } "
            "return sum; }", 13)
    program("func sum<T int | float>(a T[]) T { result := zero(T); "
            "for (i, item := range a) { result += item; } return result; } "
            "func main() int { return sum<int>(int[]{4, 6}); }", 10)
    program("func main() int { n := 0; outer: for (i := range int[]{1, 2}) { "
            "for (j, item := range int[]{3, 4}) { n += item; break outer; } } return n; }", 3)

    # Blank bindings discard either slot without declaring a variable.
    program("struct Row { value int; } func main() int { sum := 0; "
            "for (_, row := range Row[]{{3}, {5}}) { sum += row.value; } "
            "for (i, _ := range int[]{4, 6}) { sum += i; } "
            "for (_ := range \"AB\") { sum++; } "
            "for (_, _ := range int[]{1, 2}) { sum++; } return sum; }", 13)
    program('func main() int { sum := 0; a := map[string]string{"ab": "xyz"}; '
            'for (_, value := range a) { sum += len(value); } '
            'for (key, _ := range a) { sum += len(key); } '
            'for (_, _ := range a) { sum++; } return sum; }', 6)

    # Assigning to the visible index must not change traversal or item selection.
    program("func main() int { sum := 0; for (i, item := range int[]{3, 5, 7}) { "
            "sum += i + item; i = 100; sum += i; } return sum - 300; }", 18)
    program("func main() int { sum := 0; for (i := range int[]{3, 5, 7}) { "
            "sum += i; i = -1; continue; } return sum; }", 3)
    program("func main() int { sum := 0; outer: for (i, item := range int[]{3, 5, 7}) { "
            "for (j := range int[]{1}) { sum += i + item; p := &i; *p = 100; "
            "continue outer; } } return sum; }", 18)

    source.write_text("package main; func main() { for (i := range 42) {} }")
    rejected = run([compiler, "build", str(source), "-o", str(exe)])
    assert rejected.returncode != 0 and "range requires an array" in rejected.stderr, rejected


print("Range loop runtime checks passed")
