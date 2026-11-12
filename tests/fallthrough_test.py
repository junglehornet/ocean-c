"""Implicit zero returns, reachability warnings, and source diagnostic styling."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
base_env = {**os.environ, "NO_COLOR": "1"}


def run(*args, env=base_env):
    return subprocess.run(args, text=True, capture_output=True, timeout=20, env=env)


with tempfile.TemporaryDirectory(prefix="ocean fallthrough ") as directory:
    root = Path(directory)
    source, ir, executable = root / 'fallthrough % "source".oc', root / "main.ll", root / "program"

    def program(text, warnings, status=0):
        text = "package main;\n" + text
        source.write_text(text)
        for options in (("--check",), ("-o", str(executable)), ("--emit-llvm", "-o", str(ir))):
            result = run(compiler, "build", str(source), *options)
            assert result.returncode == 0, result.stdout + result.stderr
            assert result.stderr.count("semantic warning:") == warnings, result.stderr
            assert "\033[" not in result.stderr, result.stderr
            if options[0] == "-o":
                execution = run(str(executable))
                assert (execution.returncode, execution.stdout, execution.stderr) == (status, "", ""), execution
        for optimization in ("-O0", "-O2"):
            built = run(clang, optimization, "-x", "ir", str(ir), "-o", str(executable), "-lffi")
            assert built.returncode == 0, built.stderr
            execution = run(str(executable))
            assert (execution.returncode, execution.stdout, execution.stderr) == (status, "", ""), execution

    program("func main() int {}", 1)
    program("func choose(flag bool) int { if (flag) return 7; }\n"
            "func main() int { return choose(false) + choose(true); }", 1, 7)
    program("func i() int {} func b() bool {} func f() float {} func p() *int {}\n"
            "func main() int { if (i() != 0 || b() || f() != 0.0 || p() != zero(*int)) return 1; return 0; }", 4)
    program("struct Result { n int; text string; items int[]; }\n"
            "func result() Result {} func values() (int, string, int[]) {}\n"
            "func main() int { r := result(); n, s, xs := values(); "
            "return r.n + len(r.text) + len(r.items) + n + len(s) + len(xs); }", 2)
    program("func text() string {} func array() int[] {} func fixed() buffer<string>[2] {}\n"
            "func main() int { a := array(); b := fixed(); "
            "return len(text()) + len(a) + len(b[0]) + len(b[1]); }", 3)
    program("struct Factory<T> {} func (f Factory<T>) make() T {} func make<T>() T {}\n"
            "func main() int { f := Factory<string>{}; s := f.make(); "
            "return make<int>() + len(make<string>()) + len(s); }", 2)
    program("func named() (int value) { value = 9; }\n"
            "func main() int { return named(); }", 1)
    program("func arrays(flag bool) string { if (flag) return \"x\"; }\n"
            "func main() int { return len(arrays(true)) + len(arrays(false)); }", 1, 1)
    program("func main() {}", 0)
    program("func a(flag bool) int { if (flag) return 3; else return 4; }\n"
            "func b() int { if (true) return 1; }\n"
            "func c() int { panic \"unused\"; }\n"
            "func d() int { for (;;) {} }\n"
            "func e() int { goto done; done: return 2; }\n"
            "func main() int { return a(true) + b() + e(); }", 0, 6)
    program("func a() int { goto done; return 9; done: {} }\n"
            "func b() int { for (;;) { break; } }\n"
            "func c(xs int[]) int { for (i := range xs) return i; }\n"
            "func main() int { return a() + b() + c(int[]{}); }", 3)
    program("func a(x any) int { switch (x.(type)) { case int: return 1; } }\n"
            "func b(x any) int { switch (x.(type)) { case int: return 2; default: return 3; } }\n"
            "func c(x any) int { switch (x.(type)) { case int | any: return 4; } }\n"
            "func d(x any) int { switch (x.(type)) { default: break; } }\n"
            "func main() int { return a(true) + b(true) + c(true) + d(true); }", 2, 7)

    program("func noExit() int { again: goto again; } "
            "func choose(x any) int { switch (x.(type)) { default: {} case any: return 3; } } "
            "func main() int { return choose(true); }", 0, 3)
    # Exhaustive enum matches have no unmatched path, including when nested.
    program('''enum Operation { Negate }
    func negate(res Result<float>, op Operation) Result<float> {
        switch (op) {
            case Operation.Negate:
                match (res) {
                    Result<float>.error => return res;
                    Result<float>.value => return Result<float>.value(-res.unwrap());
                }
            default: panic "unknown operation";
        }
    }
    func main() int {
        result := negate(Result<float>.value(3), Operation.Negate);
        if (result.unwrap() != -3) return 1;
        result = negate(Result<float>.error("expected"), Operation.Negate);
        if (!result.hasError() || result.error() != "expected") return 2;
        return 0;
    }''', 0)
    # Covering all variants does not imply that all branch bodies return.
    program('''enum Choice { A, B }
    func empty(c Choice) int { match(c) { Choice.A => return 4; Choice.B => {} } }
    func conditional(c Choice, flag bool) int { match(c) { Choice.A => return 3; Choice.B => { if (flag) return 2; } } }
    func main() int { return empty(Choice.A) + empty(Choice.B) + conditional(Choice.A, false) + conditional(Choice.B, false); }
    ''', 2, 7)
    # Missing variants still warn about both coverage and possible fallthrough.
    program('''enum Choice { A, B }
    func partial(c Choice) int { match(c) { Choice.A => return 4; } }
    func main() int { return partial(Choice.A) + partial(Choice.B); }
    ''', 2, 4)
    program('''enum Choice { A, B }
    func fallback(c Choice) int { match(c) { Choice.A => return 4; Choice => return 3; } }
    func main() int { return fallback(Choice.A) + fallback(Choice.B); }
    ''', 0, 7)

    # A generic declaration warns once, independent of specialization/call count.
    text = 'package main;\nfunc missing<T>() T {\n}\nfunc main() int { return missing<int>(); }\n'
    source.write_text(text)
    colored_env = {key: value for key, value in os.environ.items() if key != "NO_COLOR"}
    colored_env["CLICOLOR_FORCE"] = "1"
    warned = run(compiler, "build", "--check", str(source), env=colored_env)
    assert warned.returncode == 0
    assert "\033[33m" in warned.stderr and "\033[31m" not in warned.stderr, warned.stderr
    plain = re.sub(r'\x1b\[[0-9;]*m', '', warned.stderr)
    assert f'[{os.path.relpath(source)}:2:6]: semantic warning:' in plain, plain
    assert ' 2 | func missing<T>() T {' in plain and '\n   |      ^\n' in plain, plain

    uncolored = run(compiler, "build", "--check", str(source), env={**colored_env, "NO_COLOR": "1"})
    assert uncolored.returncode == 0 and "\033[" not in uncolored.stderr
    source.write_text("func main() {}")
    package_error = run(compiler, "build", str(source), env=colored_env)
    assert package_error.returncode != 0 and "\033[31m" in package_error.stderr
    assert "package error:" in package_error.stderr and " 1 | func main() {}" in package_error.stderr

    # Scanner, parser, semantic, and backend failures share red source diagnostics.
    for text, phase in (("func main() { @ }", "scanner"),
                        ("func main() { x := ; }", "syntax"),
                        ("func main() { x: bool = 1; }", "semantic"),
                        ("func main() { goto missing; }", "backend")):
        source.write_text("package main;\n" + text)
        failed = run(compiler, "build", str(source), "-o", str(executable), env=colored_env)
        assert failed.returncode != 0, failed
        assert "\033[31m" in failed.stderr and f"{phase} error:" in failed.stderr, failed.stderr
        assert " 2 | " + text in failed.stderr and "^" in failed.stderr, failed.stderr

print("implicit zero returns and diagnostic warnings passed at -O0 and -O2")
