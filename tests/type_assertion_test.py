"""Exercise single- and two-value boxed type assertions through native LLVM."""
import os
import pathlib
import subprocess
import sys
import tempfile

compiler, clang = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
source_text = '''package main;
interface Named { func name() string; }
struct Dog { n string; age int; }
func (d Dog) name() string { return d.n; }
func describe(value any) int {
    if (s, ok := value.(string); ok) { return len(s); }
    n, ok := value.(int);
    if (ok) return n * 100;
    return -1;
}
func main() int {
    value: any = "hello";
    msg, ok := value.(string);
    if (!ok || msg != "hello") return 1;
    n, isInt := value.(int);
    if (isInt || n != 0) return 2;
    num: any = 42;
    n, isInt = num.(int);
    if (!isInt || n != 42) return 3;
    msg, ok = num.(string);
    if (ok || msg != "") return 4;
    _, ok = value.(string);
    if (!ok) return 5;
    a: any = Dog{"rex", 3};
    d, okd := a.(Dog);
    if (!okd || d.n != "rex" || d.age != 3) return 6;
    named: Named = Dog{"fido", 1};
    d2, ok2 := named.(Dog);
    if (!ok2 || d2.n != "fido") return 7;
    b: any = int[]{1, 2, 3};
    xs, okx := b.(int[]);
    if (!okx || len(xs) != 3 || xs[2] != 3) return 8;
    ys, oky := value.(int[]);
    if (oky || len(ys) != 0) return 9;
    f, okf := num.(float);
    if (okf || f != 0.0) return 10;
    if (describe("abc") != 3 || describe(7) != 700 || describe(1.5) != -1) return 11;
    if (value.(string) != "hello") return 12;
    items: any[] = int[]{1, 2};
    boxed: any = items;
    view, okv := boxed.(any[]);
    if (!okv || len(view) != 2) return 13;
    raw: any = int[]{5, 6, 7};
    rawView, okr := raw.(any[]);
    if (!okr || len(rawView) != 3) return 14;
    none, okn := num.(any[]);
    if (okn || len(none) != 0) return 15;
    return 0;
}
'''
trap_text = '''package main;
func main() int {
    value: any = 1;
    text := value.(string);
    return len(text);
}
'''
mismatch_text = '''package main;
func main() int {
    value: any = 1;
    number, ok: int = value.(int);
    return number + ok;
}
'''
with tempfile.TemporaryDirectory(prefix="ocean type assertions ") as directory:
    root = pathlib.Path(directory)
    env = dict(os.environ, OCEAN_BUILD_CACHE="off")

    def run(*args, expect=0):
        result = subprocess.run(args, capture_output=True, text=True, timeout=30, env=env)
        ok = result.returncode == 0 if expect == 0 else result.returncode != 0
        assert ok, result.stdout + result.stderr + f"\nexit: {result.returncode}"
        return result

    program = root / "program"
    program.mkdir()
    (program / "main.oc").write_text(source_text)
    executable, ir = root / "program.out", root / "program.ll"
    run(compiler, "check", str(program))
    run(compiler, "build", str(program), "-o", str(executable))
    run(str(executable))
    run(compiler, "build", str(program), "--emit-llvm", "-o", str(ir))
    run(clang, "-O2", "-x", "ir", str(ir), "-o", str(executable), "-lffi")
    run(str(executable))

    # The single-value form still traps on a mismatched dynamic type.
    trap = root / "trap"
    trap.mkdir()
    (trap / "main.oc").write_text(trap_text)
    run(compiler, "build", str(trap), "-o", str(root / "trap.out"))
    run(str(root / "trap.out"), expect=1)

    # The success result is always bool.
    mismatch = root / "mismatch"
    mismatch.mkdir()
    (mismatch / "main.oc").write_text(mismatch_text)
    result = run(compiler, "check", str(mismatch), expect=1)
    assert "expected int, got bool" in result.stdout + result.stderr, result.stdout + result.stderr
