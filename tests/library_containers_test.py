"""Regressions discovered while implementing JSON, HTTP, and process libraries."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(p).resolve()) for p in sys.argv[1:])
env = dict(os.environ, OCEAN_BUILD_CACHE="off")

def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=30, env=env)

with tempfile.TemporaryDirectory(prefix="ocean library containers ") as directory:
    root = Path(directory)
    source, ir, native = (root / name for name in ("main.oc", "module.ll", "native"))

    def program(body, trap=False):
        source.write_text("package main;\n" + body)
        result = run(compiler, "build", source, "--emit-llvm", "-o", ir)
        assert result.returncode == 0, result.stderr
        for optimization in ("-O0", "-O2"):
            result = run(clang, optimization, "-Wno-override-module", ir, "-lffi", "-o", native)
            assert result.returncode == 0, result.stderr
            result = run(native)
            assert (result.returncode != 0 if trap else result.returncode == 0), (body, result)

    program('''
struct Number { Text string; }
func value(depth int) Result<any> {
    if (depth == 0) return Result<any>.value(Number{"one"});
    items: mixed any[] = {};
    for (i := 0; i < 2; i++) {
        r := value(depth - 1); item := r.unwrap(); items.append(item);
    }
    return Result<any>.value(items);
}
func main() int {
    boxed := value(2).unwrap(); outer := boxed.(mixed any[]);
    inner := outer[0].(mixed any[]);
    if (len(outer) != 2 || inner[1].(Number).Text != "one") return 1;
    copy := inner; removed := inner.pop();
    if (removed.(Number).Text != "one") return 3;
    inner[0] = "changed";
    if (copy[0].(Number).Text != "one") return 2;
    obj := map[string]any{"items": boxed};
    result := Result<any>.value(obj).unwrap();
    saved := result.(map[string]any);
    return len(saved["items"].(mixed any[])) - 2;
}''')
    program('''
interface Item { Get() int; }
struct A { N int; }
struct B { N int; }
func (a A) Get() int { return a.N; }
func (b B) Get() int { return b.N; }
func relay(items ...Item) int { return len(items); }
func main() int {
    items: mixed Item[] = {};
    items.append(A{1}); items.prepend(B{2}); items.add(1, A{3});
    if (len(items) != 3 || items[1].Get() != 3 || relay(items...) != 3) return 1;
    removed := items.pop();
    if (removed.Get() != 1) return 2;
    uniform: any[] = {}; uniform.append(1); uniform.append(2);
    return len(uniform) - 2;
}''')
    program('''func main() { values := any[]{1, 2}; values[0] = true; }''', trap=True)
    program('''func main() { values := any[]{1, 2}; values.append(true); }''', trap=True)

    # Package rewriting must not let ordinary field names shadow type names.
    helper = root / "headers"
    helper.mkdir()
    (helper / "headers.oc").write_text('''package headers;
struct Header { Value int; }
struct Request { Header Header; Other Header; }
func New() Request { return Request{Header{1}, Header{2}}; }
''')
    (root / "ocean.mod").write_text("module example.com/library-regression\nocean 0.1\n")
    program('''import "headers";
func main() int { r := headers.New(); return r.Header.Value + r.Other.Value - 3; }''')

    for fields in ("Items Node[];", "Items map[string]Node;", "Items Node[]; Entries map[string]Node;"):
        source.write_text("package main; struct Node { " + fields + " } func main() { n: Node; }")
        result = run(compiler, "check", source)
        assert result.returncode > 0 and "recursive array-value layouts" in result.stderr, result
    source.write_text("package main; struct Node<T> { Items Node<T>[]; } func main() { n: Node<int>; }")
    result = run(compiler, "check", source)
    assert result.returncode > 0 and "recursive array-value layouts" in result.stderr, result
print("library container regressions passed")
