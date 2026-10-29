"""Distinct definitions, transparent aliases, casts, constraints and type identity."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(lambda p: str(Path(p).resolve()), sys.argv[1:])

def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=20)

with tempfile.TemporaryDirectory(prefix='ocean defined types ') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def program(body, status=0):
        source.write_text('package main;\n' + body)
        check = run(compiler, 'build', '--check', str(source))
        assert check.returncode == 0, check.stderr
        built = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert built.returncode == 0, built.stderr
        for optimization in ('-O0', '-O2'):
            built = run(clang, optimization, '-Wno-override-module', '-x', 'ir', str(ir), '-o', str(executable), '-lffi')
            assert built.returncode == 0, built.stderr
            result = run(str(executable))
            assert result.returncode == status, (body, result)

    def rejected(body, message='type mismatch'):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', '--check', str(source))
        assert result.returncode != 0 and message in result.stderr, result

    program('''type cStr buffer<i8>; type Alias = cStr;
func use(s cStr) int { return s[0]->int; }
func main() int {
    bytes := buffer<i8>[2]{65, 0}; raw: buffer<i8> = bytes;
    named := raw->cStr; alias: Alias = named; back := alias->buffer<i8>;
    if (back[0] != 65) return 1;
    return use(alias);
}''', 65)

    program('''type Count int; type Alias = Count; type Other int;
func twice<T ~int>(x T) T { return x + x; }
func identity<T any>(x T) T { return x; }
func classify(x any) int { switch (v := x.(type)) {
case Count: return v->int; case int: return 20; case Other: return 30; default: return 99; } }
func underlying(x any) int { switch (v := x.(type)) { case ~int: return v; default: return 99; } }
func main() int {
    n := 3->Count; n++; n += 1->Count; a: Alias = identity<Count>(n);
    if (zero(Count)->int != 0 || sizeof(Count) != sizeof(int)) return 1;
    return twice<Count>(a)->int + classify(a) + classify(1) + classify(1->Other) + underlying(a);
}''', 70)

    program('''type Small i16; type Single f32;
func main() int { x := (-7)->Small; ++x; y := 1.5->Single; y++;
    box: any = y; z := box.(Single);
    if (sizeof(Small) != 2 || sizeof(Single) != 4 || z->float != 2.5) return 1;
    return (-x)->int + (z->int); }''', 8)

    program('''struct Record { value int; text string; }
type Named Record; type Numbers int[]; type Fixed buffer<int>[2];
func main() int {
    n := Named{value: 3, text: "x"}; raw := n->Record; raw.value = 9;
    if (n.value != 3) return 1;
    arr := Numbers{1, 2}; copy := arr; arr += 3;
    plain := arr->(int[]); plain[0] = 9;
    fixed := Fixed{4, 5}; b := fixed->(buffer<int>[2]);
    return len(copy) + len(arr) + arr[0] + b[1] + n.value;
}''', 14)

    program('''func take(x Future) Future { return x; }
func main() int { x: Alias = 4->Future; return take(x)->int; }
type Alias = Future; type Future int;''', 4)

    program('''type Bytes = buffer<i8>; type Count = int;
func read(b Bytes, n Count) int { return b[0]->int + n; }
func main() int { raw := buffer<i8>[1]{7}; view: buffer<i8> = raw;
    alias: Bytes = view; back: buffer<i8> = alias;
    return read(back, 3); }''', 10)

    program('''type Global int;
func main() int { type Local = Global; type Distinct Global;
    x: Local = 3->Global; y := x->Distinct;
    { type Global = bool; b: Global = true; if (!b) return 1; }
    return y->Global->int; }''', 3)

    program('''type Link *Link;
func main() int { x := zero(Link); p: *Link = &x;
    y := p->Link; q := y->(*Link); return sizeof(Link) + (q == p ? 1 : 0); }''', 9)

    for body in (
        'type C buffer<i8>; func f(x C) {} func main() { b: buffer<i8>; f(b); }',
        'type C buffer<i8>; func f(x buffer<i8>) {} func main() { b := zero(C); f(b); }',
        'type C buffer<i8>; func main() { raw: buffer<i8>; x: C = raw; }',
        'type C buffer<i8>; func main() { x := zero(C); raw: buffer<i8> = x; }',
        'type C buffer<i8>; func f(x C) buffer<i8> { return x; }',
        'type Count int; func main() { x := 3->Count; n: int = x; }',
        'type Count int; func main() { x: Count = 3; }',
        'type A int; type B int; func main() { x: A = 1->B; }',
        'type Count int; func main() { x := 3->Count; x += 1; }',
        'type Count int; func f<T int>(x T) {} func main() { f<Count>(3->Count); }',
    ):
        rejected(body, 'constraint' if 'f<T int>' in body else 'type mismatch')
    rejected('type A B; type B A; func main() {}', 'cyclic')
    rejected('type A = B; type B = A; func main() {}', 'cyclic')
    rejected('type A = *A; func main() {}', 'cyclic')
    rejected('type A void; func main() {}', 'void')

    program('''type Data any;
func classify(x any) int { switch (v := x.(type)) {
case Data: raw := v->any; return raw.(int); default: return 99; } }
func main() int { x := 7->Data; y: any = x; return classify(y); }''', 7)

    program('''struct Box<T> { value T; } type Alias = Box; type Ints = int[];
func main() int { a := Alias<int>{3}; b: Box<int> = a; nums := Ints{1, 2};
    raw: int[] = nums; return b.value + raw[1]; }''', 5)

    program('''func f<T any>(x T) T { type Local T; n := x->Local; return n->T; }
func main() int { if (!f<bool>(true)) return 1; return f<int>(9); }''', 9)

    program('''type Count int; type Alias = Count;
func (c Count) Get() int { return c->int; }
func (c *Count) Add(n Count) { *c += n; }
func (c Alias) Double() Count { return c + c; }
interface Getter { Get() int; }
func main() int { x := 3->Count; x.Add(2->Alias); p := &x;
    xs := mixed Getter[]{x, p};
    return x.Double().Get() + p.Get() + len(xs); }''', 17)

    program('''type P *int;
func (p P) Read() int { return *p; }
func (p *P) Clear() { *p = null; }
func main() int { n := 7; p := (&n)->P; result := p.Read(); p.Clear();
    return result + (p == null ? 1 : 0); }''', 8)

    program('''struct Base { n int; }
func (b Base) Get() int { return b.n; }
type Alias = Base; type Distinct Base;
func (d Distinct) Own() int { return d.n + 1; }
func main() int { a := Alias{3}; b: Base = a; d := b->Distinct;
    return a.Get() + d.Own(); }''', 7)

    program('''type Int int; type Float float; type Tiny i8;
extern i32 snprintf(buffer<i8>[] out, int size, buffer<i8>[] format, ...);
func format(xs ...any) string { output := i8[40];
    size := extern.snprintf(output.data, len(output), "%d %.1f\\0".data, xs...);
    s := ""; for (i := 0; i < size; i++) s += output[i]; return s; }
func main() int { s := format((-7)->Tiny, 1.5->Float);
    if (s != "-7 1.5") return 1; return 0; }''')

    for body in (
        'type Count int; func main() { xs: int[] = Count[]{1->Count}; }',
        'type Count int; func main() { xs: Count[] = int[]{1}; }',
        'type Count int; struct Box<T> { v T; } func main() { x: Box<int> = Box<Count>{1->Count}; }',
        'type Data any; func main() { x: any = 1; y: Data = x; }',
    ):
        rejected(body)
    rejected('''struct Base {} func (b Base) Get() int { return 1; }
type Distinct Base; func main() { x := Distinct{}; x.Get(); }''', 'unknown field')
    rejected('''type A int; type Alias = A; func (a A) Get() int { return 1; }
func (a Alias) Get() int { return 2; } func main() {}''', 'duplicate method')

    program('''struct Node { value int; next *Named; } type Named Node;
struct Holder { item Named; }
func main() int { tail := Named{value: 3, next: null};
    head := Named{value: 4, next: &tail}; holder := Holder{head};
    return holder.item.value + holder.item.next.value; }''', 7)
    rejected('type Count int; func main() { x: Count<int>; }', 'type arguments')

    program('''type Values mixed any[];
func classify(x any) int { switch (v := x.(type)) {
case Values: return len(v); default: return 99; } }
func main() int { a := Values{1, "x"}; b: any = a;
    c := b.(Values); return classify(b) + len(c); }''', 4)

    def traps(body):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', str(source), '-o', str(executable))
        assert result.returncode == 0, result.stderr
        result = run(str(executable))
        assert result.returncode != 0, (body, result)

    traps('type Count int; func main() { boxed: any = 3->Count; x := boxed.(int); }')
    traps('type Values mixed any[]; func main() { boxed: any = mixed any[]{1, "x"}; x := boxed.(Values); }')
    traps('type Values mixed any[]; func main() { boxed: any = Values{1, "x"}; x := boxed.(int[]); }')

    # Public definitions retain identity across package boundaries; aliases erase it.
    (root / 'ocean.mod').write_text('module example.com/types\nocean 0.1\n')
    library = root / 'types'
    library.mkdir()
    (library / 'types.oc').write_text('''package types;
type Count int; type Alias = Count; type Raw = int;
func Get() Count { return 4->Count; }
func (c Count) Value() int { return c->int; }
''')
    program('''import "example.com/types/types";
func main() int { x: types.Alias = types.Get(); raw: types.Raw = 3;
    y := raw->(types.Count); return x.Value() + y.Value(); }''', 7)
    rejected('''import "example.com/types/types";
func main() { raw: int = types.Get(); }''')
