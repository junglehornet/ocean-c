"""Native interface dispatch, specialization, ownership, defers and package caches."""
import os
import pathlib
import subprocess
import sys
import tempfile

compiler, clang = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
source = '''package main;
interface Value { Get() int; }
interface Embedded { Value; }
interface Factory { Make() Value; }
struct Producer {}
func (p Producer) Make() Value { return Other{n:21}; }
interface Change { Add(n int); Get() int; }
interface Generic { Scale<T int | float>(n T) float; }
interface Many { Sum(values ...int) int; }
interface Text { Text() string; }
interface Pair { Pair() (int, string); }
interface Number { Get() int; }
interface FloatValue { Get() float; }
type Count int;
type Small f32;
struct Holder<T int> { value T; }
func (c Count) Get() int { return c -> int; }
func (s Small) Get() float { return s -> float; }
func (h Holder<T>) Get() int { return h.value; }
func (b Box) Pair() (int, string) { return b.n, "pair"; }
struct Box { n int; }
struct Other { n int; }
struct Words { text string; }
func (b Box) Get() int { return b.n; }
func (b Other) Get() int { return b.n*2; }
func (b *Box) Add(n int) { b.n += n; }
func (b Box) Scale<U float | int>(n U) float { return (b.n -> float)*(n -> float); }
func (b Box) Sum(values ...int) int {
    sum := b.n; for (i := 0; i < len(values); i++) { sum += values[i]; } return sum;
}
func (w Words) Text() string { return w.text; }
func read(v Value) int { return v.Get(); }
func makeValue() Value { return Other{n:7}; }
func text(v Text) string { return v.Text(); }
func deferred(v Change) { defer v.Add(3); }
func forward<T Value>(v T) int { return v.Get(); }
func main() int {
    embedded: Embedded = Box{n:11};
    if (embedded.Get()!=11) return 8;
    a: Value = Box{n:12}; b: Value = Other{n:9};
    if (a.Get()!=12 || read(b)!=18 || makeValue().Get()!=14) return 1;
    xs := mixed Value[]{Box{n:2}, Other{n:3}};
    if (xs[0].Get()!=2 || xs[1].Get()!=6) return 2;
    concrete := Box{n:5}; p: Change = &concrete;
    p.Add(2); deferred(p);
    if (p.Get()!=10 || concrete.n!=10) return 3;
    g: Generic = Box{n:4};
    if (g.Scale<int>(3)!=12.0 || g.Scale<float>(2.5)!=10.0) return 4;
    many: Many = Box{n:1}; values := int[]{2,3};
    if (many.Sum(2,3)!=6 || many.Sum(values...)!=6 || many.Sum()!=1) return 5;
    if (text(Words{text:"Ocean"})!="Ocean") return 6;
    entries := map[string]Value{"a":Box{n:8}};
    if (entries["a"].Get()!=8 || forward<Box>(concrete)!=10) return 7;
    factory: Factory = Producer{};
    if (factory.Make().Get()!=42) return 10;
    n: Number = 13 -> Count; f: FloatValue = 1.5 -> Small;
    holder: Value = Holder<int>{value:17}; pair: Pair = concrete;
    number, label := pair.Pair();
    if (n.Get()!=13 || f.Get()!=1.5 || holder.Get()!=17 || number!=10 || label!="pair") return 9;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ocean-interface-dispatch-") as directory:
    root = pathlib.Path(directory)
    (root / "main.oc").write_text(source)
    executable, ir = root / "program", root / "program.ll"
    env = dict(os.environ, OCEAN_BUILD_CACHE="off")

    def run(*args):
        result = subprocess.run(args, capture_output=True, text=True, timeout=40, env=env)
        assert result.returncode == 0, result.stdout + result.stderr + f"\n{args}: {result.returncode}"

    run(compiler, "check", str(root))
    run(compiler, "build", str(root), "-o", str(executable))
    run(str(executable))
    run(compiler, "build", str(root), "--emit-llvm", "-o", str(ir))
    run(clang, "-O2", "-x", "ir", str(ir), "-o", str(executable), "-lffi")
    run(str(executable))
    # A zero interface has no method implementation and must fail at invocation.
    (root / "main.oc").write_text('''package main;
interface Value { Get() int; }
func main() int { v: Value; return v.Get(); }
''')
    run(compiler, "build", str(root), "-o", str(executable))
    failed = subprocess.run([str(executable)], capture_output=True, timeout=10, env=env)
    assert failed.returncode != 0
    # Adapters belong to support; package declarations and cache hits must agree.
    (root / "ocean.mod").write_text("module example.test/dispatch\n\nocean 0.1\n")
    package = root / "reader"
    package.mkdir()
    (package / "reader.oc").write_text('''package reader;
interface Value { Get() int; }
func Read(v Value) int { return v.Get(); }
''')
    (root / "main.oc").write_text('''package main;
import "example.test/dispatch/reader";
struct Box { n int; }
func (b Box) Get() int { return b.n; }
func main() int { if (reader.Read(Box{n:42})!=42) return 1; return 0; }
''')
    env.update(OCEAN_BUILD_CACHE=str(root / "cache"))
    for _ in range(2):
        run(compiler, "build", str(root), "-o", str(executable))
        run(str(executable))
print("interface method dispatch passed")
