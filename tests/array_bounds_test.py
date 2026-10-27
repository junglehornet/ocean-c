"""Exercise embedded bounds diagnostics in native and independently optimized IR builds."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(os.path.abspath, sys.argv[1:])


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=15)


with tempfile.TemporaryDirectory(prefix="ocean bounds ") as directory:
    root = Path(directory)
    source = root / 'bounds % "source".oc'
    native = root / "native"
    optimized = root / "optimized"
    ir = root / "module.ll"
    checks = 0

    def failure(text, index, length, marker, focus, frames, message=None):
        global checks
        source.write_bytes(text.encode())
        built = run([compiler, str(source), "-o", str(native)])
        assert built.returncode == 0, built.stdout + built.stderr
        emitted = run([compiler, "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        built = run([clang, "-Wno-override-module", "-O2", "-x", "ir", str(ir), "-o", str(optimized)])
        assert built.returncode == 0, built.stderr
        lines = text.splitlines()
        number, line = next((i, line) for i, line in enumerate(lines, 1) if marker in line)
        column = line.index(focus) + (focus.index("[") if "[" in focus else 0) + 1
        # Diagnostics must use the original compilation source, not read the current file.
        source.write_text("source replaced after compilation\n")
        reports = []
        for executable in [native, optimized]:
            if executable == optimized:
                source.unlink(missing_ok=True)
            result = run([str(executable)])
            assert result.returncode == 1, result
            assert result.stdout == "", result
            report = result.stderr
            assert (message or f"index {index}, length {length}") in report, report
            assert f"{source}:{number}:{column}" in report, report
            snippet = f" {number} | {line}"
            assert snippet in report, report
            caret = report.split(snippet + "\n", 1)[1].splitlines()[0]
            assert caret.index("^") == len(f" {number} | ") + column - 1, report
            assert "Call trace (most recent call first):" in report, report
            trace = [entry for entry in report.splitlines() if entry.startswith("  at ")]
            assert len(trace) == len(frames), report
            for entry, name in zip(trace, frames):
                assert entry.startswith(f"  at {name} ("), report
                assert str(source) in entry, report
            assert trace[0] == f"  at {frames[0]} ({source}:{number}:{column})", report
            reports.append(report)
        assert reports[0] == reports[1], reports
        checks += 1

    failure("\nfunc leaf(a int[], i int) int {\n    return a[i];\n}\n"
            "func middle(a int[]) int {\n    return leaf(a, 3);\n}\n"
            "func main() int {\n    a := int[3];\n    return middle(a);\n}\n",
            3, 3, "return a[i]", "a[i]", ["leaf", "middle", "main"])
    failure("func main() int { a := int[2]; return a[-1]; }", -1, 2,
            "return", "a[-1]", ["main"])
    failure("func main() int { a := int[0]; return a[0]; }", 0, 0,
            "return", "a[0]", ["main"])
    failure('extern i32 puts(buffer<i8>[] s);\nfunc main() { a := int[1]; a[1] = puts("must not execute\\0".data); }',
            1, 1, "must not execute", "a[1]", ["main"])
    failure("func main() { a := int[2]; a[2]++; }", 2, 2,
            "a[2]++", "a[2]", ["main"])
    failure("func main() { a := i32[1]; a[1] += 2; }", 1, 1,
            "a[1]", "a[1]", ["main"])
    failure("func main() int {\n    a: int[][] = int[][1];\n    a[0] = int[2];\n    return a[0][2];\n}",
            2, 2, "return", "[2]", ["main"])
    failure("func main() int { a: int[][] = int[][1]; return a[1][0]; }", 1, 1,
            "return", "a[1]", ["main"])
    failure("func recur(a int[], n int) int { if (n == 0) { return a[4]; } return recur(a, n - 1); }\n"
            "func main() int { a := int[2]; return recur(a, 3); }", 4, 2,
            "return a[4]", "a[4]", ["recur", "recur", "recur", "recur", "main"])
    failure("\r\nfunc main() int {\r\n\ta := int[1];\r\n\treturn a[2];\r\n}\r\n", 2, 1,
            "return", "a[2]", ["main"])
    failure("func main() int {\n    a := int[1];\n    return a[\n        2\n    ];\n}", 2, 1,
            "return a[", "a[", ["main"])
    failure("func done() {} func main() int { done(); a := int[1]; return a[3]; }", 3, 1,
            "return a[3]", "a[3]", ["main"])
    failure("func main() int { a := int[1]; b := int[2]; return a[(a = b, 1)]; }", 1, 1,
            "return", "a[(", ["main"])

    failure("extern i32 dprintf(i32 fd, buffer<i8>[] format, ...); extern void exit(i32 code);\n"
            "func main() int { a := int[1]; return a[1]; }", 1, 1,
            "return", "a[1]", ["main"])

    failure("func fill(a int[]) {\n    a.data = {1, 2, 3};\n}\nfunc main() { a := int[2]; fill(a); }", 3, 2,
            "a.data", "data", ["fill", "main"], message="Array data literal too large: literal length 3 exceeds array length 2")
    failure("func main() { a := string[0]; a.data = {\"x\"}; }", 1, 0,
            "a.data", "data", ["main"], message="Array data literal too large: literal length 1 exceeds array length 0")
    failure('extern i32 puts(buffer<i8>[] s); func item() int { puts("must not run\\0".data); return 1; }\nfunc main() { a := int[1]; a.data = {item(), 2}; }', 2, 1,
            "a.data", "data", ["main"], message="Array data literal too large: literal length 2 exceeds array length 1")

    failure('func main() int { s := "abc"; return s[3]; }', 3, 3,
            "return", "s[3]", ["main"])
    failure('func main() int { s := "a" + "b"; return s[-1]; }', -1, 2,
            "return", "s[-1]", ["main"])
    failure('func main() { s := "abc"; s.data = {1, 2, 3, 4}; }', 4, 3,
            "s.data", "data", ["main"], message="Array data literal too large: literal length 4 exceeds array length 3")

    failure("func read(xs ...int) int { return xs[0]; }\nfunc main() int { return read(); }", 0, 0,
            "return xs[0]", "xs[0]", ["read", "main"])
    failure("func read(xs ...int) int { return xs[2]; }\nfunc main() int { a := int[2]; return read(a...); }", 2, 2,
            "return xs[2]", "xs[2]", ["read", "main"])
    failure("func fill(xs ...int) { xs.data = {1, 2}; }\nfunc main() { a := int[1]; fill(a...); }", 2, 1,
            "xs.data", "data", ["fill", "main"], message="Array data literal too large: literal length 2 exceeds array length 1")

    failure("func main() int { return int[]{1, 2}[2]; }", 2, 2,
            "return", "[2]", ["main"])

print(f"{checks} array runtime diagnostic checks passed in native and optimized IR builds")
