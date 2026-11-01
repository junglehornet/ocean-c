"""Panic statements share source excerpts and call traces with bounds failures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=15)


with tempfile.TemporaryDirectory(prefix="ocean panic ") as directory:
    root = Path(directory)
    source = root / 'panic % "source".oc'
    ir, executable = root / "panic.ll", root / "native"
    display_source = os.path.relpath(source)
    checks = 0

    def failure(text, message, frames, marker="panic ", focus="panic "):
        global checks
        text = "package main;\n" + text
        source.write_text(text)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stderr
        line_number, line = next((i, line) for i, line in enumerate(text.splitlines(), 1)
                                 if marker in line)
        column = line.index(focus) + (focus.index("(") if "(" in focus else 0) + 1
        reports = []
        for optimization in ("-O0", "-O2"):
            built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir),
                         "-o", str(executable)])
            assert built.returncode == 0, built.stderr
            source.unlink(missing_ok=True)
            result = run([str(executable)])
            assert result.returncode == 1 and result.stdout == "", result
            report = result.stderr
            assert report.startswith("Panic: " + message + "\n"), report
            assert f"  --> {display_source}:{line_number}:{column}\n" in report, report
            snippet = f" {line_number} | {line}\n"
            assert snippet in report, report
            caret = report.split(snippet, 1)[1].splitlines()[0]
            assert caret.index("^") == len(f" {line_number} | ") + column - 1, report
            assert "Call trace (most recent call first):" in report, report
            trace = [entry for entry in report.splitlines() if entry.startswith("  at ")]
            assert len(trace) == len(frames), report
            for entry, name in zip(trace, frames):
                assert entry.startswith(f"  at {name} ("), report
            assert trace[0] == f"  at {frames[0]} ({display_source}:{line_number}:{column})", report
            reports.append(report)
            checks += 1
        assert reports[0] == reports[1], reports

    failure('func main() int {\n    panic "broken %s %n";\n}\n', "broken %s %n", ["main"])
    failure('func leaf(message string) int {\n    panic message;\n}\n'
            'func middle() int { return leaf("runtime " + "message"); }\n'
            'func main() int { return middle(); }\n', "runtime message", ["leaf", "middle", "main"])
    failure('func recur(n int) int[] {\n    if (n == 0) panic "recursive";\n'
            '    return recur(n - 1);\n}\nfunc main() { a := recur(3); }\n',
            "recursive", ["recur"] * 4 + ["main"])
    failure('func fail<T>(value T) T {\n    panic "generic";\n}\n'
            'func main() int { return fail<int>(0); }\n', "generic", ["fail", "main"])
    failure('struct S {}\nfunc (s S) fail() {\n    panic "method";\n}\n'
            'func main() { s := S{}; s.fail(); }\n', "method", ["fail", "main"])
    failure('func main() { panic ""; }', "", ["main"])
    failure('func main() { panic "a\\0b\\nend"; }', "a\0b\nend", ["main"])
    failure('func choose(flag bool) int { if (flag) panic "branch"; else return 2; }\n'
            'func main() int { return choose(true); }', "branch", ["choose", "main"])

    failure('func check(ok bool) int {\n    if (!ok) return panic "invalid";\n    return 0;\n}\n'
            'func main() int {\n    check(true);\n    return check(false);\n}\n',
            "invalid", ["check", "main"], "return check(false)", "check(")
    failure('func leaf(message string) int { return panic message; }\n'
            'func middle() int {\n    return leaf("runtime " + "message");\n}\n'
            'func main() int { return middle(); }\n', "runtime message", ["leaf", "middle", "main"],
            'return leaf(', 'leaf(')
    failure('func fail<T>(value T) T { return panic "generic"; }\n'
            'func main() int { return fail<int>(0); }\n', "generic", ["fail", "main"],
            'return fail<int>', 'fail<int>(')
    failure('struct Optional<T> { value T; ok bool; }\n'
            'func (o Optional<T>) unwrap() T { if (!o.ok) return panic "empty"; return o.value; }\n'
            'func main() int {\n    o := zero(Optional<int>);\n    return o.unwrap();\n}\n',
            "empty", ["unwrap", "main"], 'return o.unwrap()', 'unwrap(')
    failure('func make() int[] { return panic "array"; }\n'
            'func main() {\n    a := make();\n}\n', "array", ["make", "main"], 'a := make()', 'make(')
    failure('func recur(n int) int[] { if (n == 0) return panic "recursive";\n'
            '    return recur(n - 1);\n}\nfunc main() { a := recur(3); }\n',
            "recursive", ["recur"] * 4 + ["main"], 'return recur(n - 1)', 'recur(')
    failure('func pair() (int, bool) { return panic "pair"; }\n'
            'func main() { a, b := pair(); }\n', "pair", ["pair", "main"], 'a, b := pair()', 'pair(')
    failure('func stop() { return panic "stopped"; }\nfunc main() { stop(); }\n',
            "stopped", ["stop", "main"], 'stop();', 'stop(')
    failure('func main() int { return panic "entry"; }', "entry", ["main"], 'return panic', 'return')

    # Statement wrappers preserve declarations, successful values, and control flow.
    for body in (
        'forward panic return leaf();',
        'forward panic { return leaf(); }',
        'forward panic x := leaf(); return x;',
        'forward panic if (true) return leaf(); else return 0;',
        'forward panic { forward panic return leaf(); }',
    ):
        failure('func leaf() int { return panic "forwarded"; }\n'
                f'func middle() int {{ {body} }}\n'
                'func main() int { return middle(); }\n',
                "forwarded", ["leaf", "middle", "main"], 'return middle()', 'middle(')

    failure('func leaf() int { return panic "chain"; }\n'
            'func inner() int { forward panic return leaf(); }\n'
            'func outer() int { forward panic return inner(); }\n'
            'func main() int { return outer(); }\n',
            "chain", ["leaf", "inner", "outer", "main"], 'return outer()', 'outer(')
    failure('func leaf() int { return panic "stop"; }\n'
            'func inner() int { return leaf(); }\n'
            'func outer() int { forward panic return inner(); }\n'
            'func main() int { return outer(); }\n',
            "stop", ["leaf", "inner", "outer", "main"], 'return leaf()', 'leaf(')
    failure('func leaf() int { panic "local"; }\n'
            'func middle() int { forward panic return leaf(); }\n'
            'func main() int { return middle(); }\n',
            "local", ["leaf", "middle", "main"])
    failure('func middle() { forward panic { panic "hi"; } }\n'
            'func main() { forward panic middle(); }\n',
            "hi", ["middle", "main"], 'panic "hi"', 'panic "hi"')
    failure('func middle() { forward panic { return panic "helo"; } }\n'
            'func main() { middle(); }\n',
            "helo", ["middle", "main"], 'middle();', 'middle(')

    failure('func leaf(ok bool) int { if (ok) return 1; return panic "scope"; }\n'
            'func middle() int { forward panic { leaf(true); } return leaf(false); }\n'
            'func main() int { return middle(); }\n',
            "scope", ["leaf", "middle", "main"], 'return leaf(false)', 'leaf(false)')
    failure('func leaf() int { return panic "root"; }\n'
            'func main() int { forward panic return leaf(); }\n',
            "root", ["leaf", "main"], 'return leaf()', 'leaf(')
    failure('func leaf<T>(x T) T { return panic "generic forward"; }\n'
            'func middle() int { forward panic return leaf<int>(0); }\n'
            'func main() int { return middle(); }\n',
            "generic forward", ["leaf", "middle", "main"], 'return middle()', 'middle(')

    for text, error in [
        ('forward panic {} func main() {}', 'only allowed inside a function'),
        ('func main() { forward panic }', 'Expected'),

        ('func main() { panic 42; }', 'type mismatch'),
        ('func main() int { return panic 42; }', 'type mismatch'),
        ('func main() int { return panic; }', 'expected expression'),
        ('func main() int { return panic \"missing\" }', 'semicolon'),
        ('func main() { panic; }', 'Expected'),
        ('func main() { panic "missing" }', 'Expected semicolon after panic'),
        ('panic "outside"; func main() {}', 'Panic is only allowed inside a function'),
        ('func main() { panic missing; }', 'unresolved name'),
    ]:
        source.write_text("package main; " + text)
        result = run([compiler, "build", "--check", str(source)])
        assert result.returncode != 0 and error.lower() in (result.stdout + result.stderr).lower(), result

    source.write_text('package main; func fail<T>(message T) { panic message; } func main() { fail<int>(42); }')
    invalid = run([compiler, "build", str(source), "-o", str(executable)])
    assert invalid.returncode > 0 and "string" in (invalid.stdout + invalid.stderr), invalid

    source.write_text('package main; func value() int { return 3; } '
                      'func main() int { forward panic x := value(); '
                      'forward panic { x += value(); } '
                      'forward panic if (x == 6) return x + 1; else return 0; }')
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stderr
    for optimization in ("-O0", "-O2"):
        built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir),
                     "-o", str(executable)])
        assert built.returncode == 0, built.stderr
        result = run([str(executable)])
        assert result.returncode == 7 and result.stdout == "" and result.stderr == "", result

    source.write_text('package main; func main() int { if (false) panic "unreachable"; return 7; }')
    built = run([compiler, "build", str(source), "-o", str(executable)])
    assert built.returncode == 0, built.stderr
    result = run([str(executable)])
    assert result.returncode == 7 and result.stderr == "", result

print(f"{checks} panic runtime checks passed at -O0 and -O2; invalid statements rejected")
