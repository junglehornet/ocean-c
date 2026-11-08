"""Homogeneous and mixed interface arrays, including dynamic conformance checks."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(lambda p: str(Path(p).resolve()), sys.argv[1:])


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=20)


with tempfile.TemporaryDirectory(prefix='ocean mixed arrays ') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def program(body, status=0):
        source.write_text('package main;\n' + body)
        built = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert built.returncode == 0, built.stderr
        for optimization in ('-O0', '-O2'):
            built = run(clang, optimization, '-Wno-override-module', '-x', 'ir', str(ir),
                        '-o', str(executable), '-lffi')
            assert built.returncode == 0, built.stderr
            result = run(str(executable))
            assert result.returncode == status, (result, body)

    def rejected(body, message):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', '--check', str(source))
        assert result.returncode != 0 and message in result.stderr, result

    def traps(body):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', str(source), '-o', str(executable))
        assert result.returncode == 0, result.stderr
        result = run(str(executable))
        assert result.returncode != 0, (body, result)

    definitions = '''
interface Value { Get() int; }
struct A { n int; }
struct B { n int; }
struct Missing { n int; }
func (a A) Get() int { return a.n; }
func (b B) Get() int { return b.n; }
'''
    program('''func identity(xs mixed any[]) mixed any[] { return xs; }
func relay(xs ...any) mixed any[] { return xs; }
func main() int {
    xs := mixed any[]{1, "two", true}; xs[0] = "one";
    ys: mixed any[] = {2, false}; xs += ys; xs += 3.5;
    saved := xs; xs[1] = false;
    if (saved[1].(string) != "two") return 1;
    if (len(identity(xs)) != 6 || len(relay(1, "x")) != 2) return 2;
    empty := zero(mixed any[]); empty += "a"; empty += true;
    return len(empty);
}''', 2)

    program(definitions + '''
func count(xs mixed Value[]) int { return len(xs); }
func homo(xs Value[]) int { return len(xs); }
func relay(xs ...Value) int { return len(xs); }
func main() int {
    xs := mixed Value[]{A{1}, B{2}}; xs[0] = B{3}; xs += A{4};
    uniform: Value[] = {A{1}, A{2}};
    return count(xs) + count(A[]{A{1}, A{2}}) + homo(uniform) + relay(A{1}, B{2});
}''', 9)

    program('''interface Empty {} func length(xs mixed Empty[]) int { return len(xs); }
func main() int {
    a := mixed interface{}[]{1, "x", true};
    b: interface{}[] = {1, 2};
    return length(a) + len(b);
}''', 5)

    program('''interface Scalar { int | string; }
func take(xs mixed Scalar[]) int { return len(xs); }
func main() int { xs := mixed Scalar[]{1, "x"}; xs[0] = "y";
    return take(xs) + take(int[]{2, 3}); }''', 4)

    program(definitions + '''interface Extended { Get() int; Extra() bool; }
struct Both { n int; }
func (b Both) Get() int { return b.n; }
func (b Both) Extra() bool { return true; }
func main() int { a: mixed Extended[] = {Both{3}}; return len(a); }''', 1)

    program('''interface Value { Get() int; }
struct Box<T> { value T; }
func (b Box<T>) Get() T { return b.value; }
func main() int { xs := mixed Value[]{Box<int>{3}}; return len(xs); }''', 1)

    program(definitions + '''
func classify(x any) int { switch (xs := x.(type)) {
case mixed Value[]: return len(xs); default: return 20; } }
func main() int { xs: mixed Value[] = {A{1}, B{2}};
    return classify(xs) + classify(A[]{A{1}}); }''', 3)

    program('''func classify(x any) int { switch (xs := x.(type)) {
case any[]: return 1; case mixed any[]: return len(xs); default: return 20; } }
func main() int { return classify(mixed any[]{1, true}) + classify(int[]{1}); }''', 3)

    program(definitions + '''func main() int {
    box: any = mixed any[]{A{1}, B{2}};
    a := box.(mixed Value[]);
    return a[0].(A).Get() + a[1].(B).Get(); }''', 3)

    rejected('func main() { x: mixed int[] = {}; }', 'interface')
    rejected('func main() { x: mixed any = 1; }', 'mixed requires')
    rejected('func main() { x := any[]{1, true}; }', 'same type')
    rejected(definitions + 'func main() { xs: Value[] = {A{1}, B{2}}; }', 'same type')
    rejected(definitions + 'func main() { xs: mixed Value[] = {A{1}, Missing{2}}; }', 'type mismatch')
    rejected('''interface Value { Get() int; } struct Wrong {}
func (w Wrong) Get() bool { return true; }
func main() { a := mixed Value[]{Wrong{}}; }''', 'type mismatch')
    rejected('''interface Value { Get() int; } struct PointerOnly {}
func (p *PointerOnly) Get() int { return 1; }
func main() { a := mixed Value[]{PointerOnly{}}; }''', 'type mismatch')
    rejected('''interface Scalar { int | string; }
func main() { a := mixed Scalar[]{true}; }''', 'type mismatch')
    rejected('''interface Value { Get() int; } struct Box<T> { value T; }
func (b Box<T>) Get() T { return b.value; }
func main() { xs := mixed Value[]{Box<bool>{true}}; }''', 'type mismatch')

    traps(definitions + '''func main() { bad: any = Missing{1};
        xs: mixed Value[] = {A{1}, bad}; }''')
    traps(definitions + '''func main() { bad: any = Missing{1};
        xs: mixed Value[] = {A{1}}; xs[0] = bad; }''')
    traps(definitions + '''func main() { bad: any = Missing{1};
        xs: mixed Value[] = {A{1}}; xs += bad; }''')
    traps(definitions + '''func take(xs mixed Value[]) {} func main() {
        xs := mixed any[]{A{1}, Missing{1}}; take(xs); }''')
    traps(definitions + '''func take(xs Value[]) {} func main() {
        xs := mixed Value[]{A{1}, B{1}}; take(xs); }''')
    traps(definitions + '''func take(xs ...Value) {} func main() {
        bad: any = Missing{1}; take(A{1}, bad); }''')
    traps('''interface Scalar { int | string; } func main() {
        box: any = mixed any[]{1, true}; xs := box.(mixed Scalar[]); }''')

    program(definitions + '''func main() int { a := A{3}; xs := mixed Value[]{&a};
        empty := Value[0]; return len(xs) + len(empty); }''', 1)
    rejected(definitions + 'func main() { xs := Value[2]; }', 'concrete element type')
    rejected('''interface Receiver { Receive(x any) int; } struct Narrow {}
func (n Narrow) Receive(x int) int { return x; }
func main() { xs := mixed Receiver[]{Narrow{}}; }''', 'type mismatch')
    traps('''interface Receiver { Receive(x any) int; } struct Narrow {}
func (n Narrow) Receive(x int) int { return x; }
func main() { x: any = Narrow{}; xs := mixed Receiver[]{x}; }''')

    # A mixed array constraint includes homogeneous interface and concrete arrays.
    program(definitions + '''
func count<T mixed Value[]>(xs T) int { return len(xs); }
func classify(x any) int { switch (xs := x.(type)) {
case mixed Value[]: return len(xs); default: return 100; } }
func main() int {
    uniform: Value[] = {A{1}, A{2}};
    concrete := A[]{A{3}, A{4}, A{5}};
    varied := mixed Value[]{A{6}, B{7}};
    return count<Value[]>(uniform) + count<A[]>(concrete) + count<mixed Value[]>(varied)
         + classify(uniform) + classify(concrete) + classify(varied);
}''', 14)
    rejected(definitions + '''func count<T mixed Value[]>(xs T) int { return len(xs); }
func main() int { return count<Missing[]>(Missing[]{Missing{1}}); }''', 'constraint')

    (root / 'ocean.mod').write_text('module example.com/mixed\nocean 0.1\n')
    library = root / 'values'
    library.mkdir()
    (library / 'values.oc').write_text('package values;\n' + definitions)
    program('''import "example.com/mixed/values";
func main() int { xs := mixed values.Value[]{values.A{1}, values.B{2}};
    return len(xs); }''', 2)
