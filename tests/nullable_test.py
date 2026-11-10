"""Nullable interfaces and built-in Optional at O0/O2, including panic recovery."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
env = dict(os.environ, OCEAN_BUILD_CACHE="off")
with tempfile.TemporaryDirectory(prefix="ocean-nullable-") as directory:
    root = Path(directory)
    source, ir, exe = root / "main.oc", root / "main.ll", root / "program"

    def run(*args):
        return subprocess.run(args, capture_output=True, text=True, timeout=30, env=env)

    def program(body, panic=False):
        source.write_text("package main;\n" + body)
        result = run(compiler, "build", str(root), "--emit-llvm", "-o", str(ir))
        assert result.returncode == 0, result.stderr
        for opt in ("-O0", "-O2"):
            result = run(clang, opt, str(ir), "-o", str(exe), "-lffi")
            assert result.returncode == 0, result.stderr
            result = run(str(exe))
            if panic:
                assert result.returncode != 0 and "method call on null interface" in result.stderr, result
            else:
                assert result.returncode == 0, result

    program('''
interface Value { Get() int; }
struct Box {}
func (b Box) Get() int { return 4; }
func absent() Value { return null; }
func inspect(v Value) bool { return v == null; }
func main() int {
    v: Value; if (v != null || !inspect(v) || absent() != null) return 1;
    v = Box{}; if (v == null || v.Get() != 4) return 2;
    v = null; if (null != v) return 3;
    a: Optional<int> = null; z: Optional<string>;
    if (a != null || z != Optional<string>.None || Optional<int>.None != null) return 4;
    a = Optional<int>.Some(7);
    if (a == null || a.(Optional.Some) != 7 || null == a) return 5;
    b := Optional<Value>.Some(null);
    if (b == null || b.(Optional.Some) != null) return 6;
    nested := Optional<Optional<string>>.Some(Optional<string>.None);
    if (nested == null || nested.(Optional.Some) != null) return 7;
    xs := mixed Value[]{null, Box{}};
    if (xs[0] != null || xs[1].Get() != 4) return 8;
    m := map[string]Value{"empty":null};
    if (m["empty"] != null) return 9;
    match (a) { Optional.Some(n) => { if (n != 7) return 10; } Optional.None => return 11; }
    a = null; if (a != Optional<int>.None) return 12;
    return 0;
}
''')
    declarations = 'interface Value { Get() int; }\n'
    program(declarations + 'func main() int { v: Value = null; return v.Get(); }', panic=True)
    program(declarations + '''
func rescue() { p := recover(); if (p == null || p.(string) != "method call on null interface") panic "wrong recovery"; }
func invoke(v Value) { defer rescue(); v.Get(); panic "call returned"; }
func main() int { invoke(null); return 0; }
''')
    program('''
func choose<T>(value T, present bool) Optional<T> {
    if (present) return Optional<T>.Some(value);
    return null;
}
func main() int {
    a := choose<string>("hello", true); b := choose<int>(3, false);
    if (a == null || a.(Optional.Some) != "hello" || b != null) return 1;
    r := Result<Optional<int>>.value(Optional<int>.Some(42));
    o := r.unwrap(); if (o.(Optional.Some) != 42) return 2;
    return 0;
}
''')
    program('enum Optional<T> { Custom(T) } func main() int { x := Optional<int>.Custom(9); return x.(Optional.Custom) - 9; }')
    (root / "ocean.mod").write_text("module example.test/nullable\n\nocean 0.1\n")
    package = root / "values"
    package.mkdir()
    (package / "values.oc").write_text('''package values;
interface Value { Get() int; }
enum Optional<T> { Custom(T) }
func Empty() Value { return null; }
func Custom() int { v := Optional<int>.Custom(5); return v.(Optional.Custom); }
func Maybe() Result<int> { return Result<int>.value(8); }
''')
    source.write_text('''package main;
import "example.test/nullable/values";
func absent<T>() Optional<T> { return null; }
func main() int {
    x := Optional<int>.Some(4);
    if (values.Empty() != null || absent<string>() != null || x.(Optional.Some) != 4) return 1;
    if (values.Custom() != 5 || values.Maybe().unwrap() != 8) return 2;
    return 0;
}
''')
    env["OCEAN_BUILD_CACHE"] = str(root / "cache")
    for _ in range(2):
        result = run(compiler, "build", str(root), "-o", str(exe))
        assert result.returncode == 0, result.stderr
        assert run(str(exe)).returncode == 0
    source.write_text('package main; enum Other { None } func main() { x: Other = null; }')
    assert run(compiler, "check", str(root)).returncode != 0
print("nullable interface and Optional checks passed")
