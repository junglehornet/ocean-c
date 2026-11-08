"""Tagged enum construction, extraction, matching, identity, and ownership at O0/O2."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(p).resolve()) for p in sys.argv[1:])
environment = dict(os.environ)

def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=30, env=environment)

with tempfile.TemporaryDirectory(prefix='ocean enums ') as directory:
    root = Path(directory)
    source, ir, exe = root / 'main.oc', root / 'main.ll', root / 'program'

    def program(body, status=0, output=None, warning=None, traps=False):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert result.returncode == 0, result.stderr
        if warning:
            assert warning in result.stderr, result.stderr
        for opt in ('-O0', '-O2'):
            result = run(clang, opt, '-Wno-override-module', str(ir), '-o', str(exe), '-lffi')
            assert result.returncode == 0, result.stderr
            result = run(str(exe))
            if traps:
                assert result.returncode != 0, result
            else:
                assert result.returncode == status, (body, result)
            if output is not None:
                assert result.stdout == output, result

    def rejected(body, message):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', '--check', str(source))
        assert result.returncode != 0 and message in result.stderr, (body, result)

    program('''enum Event { Load, Key(i8), Click{x, y i8}, }
func handle(e Event) int {
    if (e == Event.Load) return 1;
    if (Event.Key == e) { k := e.(Event.Key); return k->int; }
    c := e.(Event.Click); return c.x->int + c.y->int;
}
func matched(e Event) int {
    match(e) {
        Event.Load => return 1;
        Event.Key(k) => { if (k == 'q') return 0; return k->int; }
        Event.Click{y, x} => return x->int + y->int;
    }
    return 0;
}
func main() int {
    load := Event.Load; a: Event = Event.Key(4); b := Event.Click{y: 3, x: 2};
    if (a != Event.Key || load == Event.Click) return 99;
    return handle(load) + handle(a) + handle(b) + matched(load) + matched(a) + matched(b);
}''', 20)

    program('''enum Color { RGB(i8, i8, i8), Empty }
func main() int {
    c := Color.RGB(1, 7, 3); _, green, _ := c.(Color.RGB);
    other: i8; _, other, _ = c.(Color.RGB);
    match(c) { Color.RGB(_, g, _) => return green->int + other->int + g->int;
               Color => return 99; }
    return 98;
}''', 21)

    program('''enum Event { Click{x, y int}, Empty }
func main() int {
    e := Event.Click{2, 5};
    clickX, clickY := e.(Event.Click{clickY = y, clickX = x});
    x := e.(Event.Click{x});
    match(e) { Event.Click{v = y} => return clickX * 10 + clickY + x + v;
               Event => return 99; }
    return 98;
}''', 32)

    program('''struct RGB { r, g, b int; }
enum Color { Red: RGB = {255, 0, 0}, Green: RGB = {0, 255, 0}, Blue := RGB{0, 0, 255}, Other(RGB) }
enum Number { One := 1, Two := 2, Other(int) }
func main() int {
    c := Color.Red; rgb := c.(Color.Red); b := Color.Blue; blue := b.(Color.Blue);
    n := Number.Two; number := n.(Number.Two);
    match (Number.Other(4)) { Number.Other(i) => return rgb.r / 255 + blue.b / 255 + number + i;
                             Number => return 99; }
    return 98;
}''', 8)

    program('''enum Data { Text(string), Values{items int[], label string}, Empty }
func make() Data { a := int[]{2, 3}; result := Data.Values{a, "hello"}; a[0] = 99; return result; }
func id<T any>(x T) T { return x; }
func main() int {
    value := id<Data>(make()); box: any = value; copy := box.(Data);
    p := copy.(Data.Values); values := Data[]{value, copy};
    match(values[1]) { Data.Values{label, items} => {
        if (label != "hello" || items[0] != 2 || p.items[1] != 3) return 99;
    } Data => return 98; }
    text := Data.Text("abc"); s := text.(Data.Text);
    return len(s) + p.items[0] + p.items[1];
}''', 8)

    program('''enum A { X, Y } enum B { X, Y }
func main() int { a := A.X; b := B.X;
    match(a) { A.X => return 3; }
    return 0;
}''', 3, warning='non-exhaustive enum match')

    program('''enum Event { A(int), B(int) }
func main() int { e := Event.A(1); b := e.(Event.B); return b; }''', traps=True)

    program('''enum Event { A, B }
calls: int;
func next() Event { calls++; return Event.B; }
func main() int { match(next()) { Event => { if (calls != 1) return 99; }
    Event.A => return 98; } return calls; }''', 1)

    program('''enum Result<T> { value(T), error(string), empty }
func unwrap<T>(r Result<T>, fallback T) T {
    match(r) { Result.value(v) => return v; Result => return fallback; }
    return fallback;
}
func make<T>(v T) Result<T> { return Result<T>.value(v); }
func main() int {
    a := make<int>(7); b := Result<string>.value("abc");
    e := Result<int>.error("bad"); n := a.(Result<int>.value);
    if (a != Result<int>.value || e != Result.error) return 99;
    text := unwrap<string>(b, "wrong");
    empty := Result<bool>.empty;
    if (empty != Result.empty) return 98;
    return unwrap<int>(a, 0) + len(text) + n;
}''', 17)

    program('''enum Pair<T, U> { fields{left T, right U}, values(T, U), empty }
func main() int {
    e := Pair<int, string>.fields{right: "hello", left: 2};
    p := e.(Pair<int, string>.fields);
    x, y := e.(Pair.fields{y = right, x = left});
    t := Pair<bool, int>.values(true, 3); b, n := t.(Pair.values);
    match(e) { Pair<int, string>.fields{right, left} => {
        if (right != y || left != p.left || !b) return 99;
    } Pair<int, string> => return 98; }
    return p.left + x + len(y) + n;
}''', 12)

    program('''enum Result<T> { value(T), error(string) }
type IntResult = Result<int>;
struct Box<T> { item T; }
func main() int {
    nested := Result<Result<int>>.value(Result<int>.value(4));
    inner := nested.(Result.value); v := inner.(Result.value);
    alias := IntResult.value(3); a := alias.(IntResult.value);
    b := Result<Box<int>>.value(Box<int>{2}); item := b.(Result.value);
    values := Result<int>[]{inner, alias};
    box: any = values[0]; copy := box.(Result<int>); result := copy.(Result.value);
    return v + a + item.item + result;
}''', 13)

    program('''enum Result<T> { value(T), empty }
func main() int {
    a := int[]{2, 3}; e := Result<int[]>.value(a); a[0] = 99;
    b := e.(Result.value); b[1] = 98;
    c := e.(Result.value);
    x := Result<any>.value(4); box := x.(Result.value); n := box.(int);
    return c[0] + c[1] + n;
}''', 9)

    program('''enum Result<T> { value(T) }
func main() int { a := Result<int>.value(3); box: any = a; b := box.(Result<bool>); return 0; }''', traps=True)

    program('''enum Box<T> { value{item T} }
func main() int { b := Box<any>.value{4}; p := b.(Box.value); return p.item.(int); }''', 4)

    program('''enum Fixed<T> { Default: T = 4 }
func main() int { e := Fixed<any>.Default; box := e.(Fixed.Default); return box.(int); }''', 4)

    program('''enum Counter<T ~int> { value(T), defaultValue := zero(T) }
func main() int {
    a := Counter<int>.value(2); n := a.(Counter.value);
    b := Counter<int>.defaultValue; z := b.(Counter.defaultValue);
    return n + z;
}''', 2)

    for body, message in [
        ('enum E<T> { V(T) } func main() { e := E.V(1); }', 'explicit type arguments'),
        ('enum E<T> { V(T) } func main() { e := E<int, bool>.V(1); }', 'type argument count mismatch'),
        ('enum E<T> { V(T) } func main() { e := E<int>.V(true); }', 'type mismatch'),
        ('enum E<T> { V(T) } func main() { e: E<int> = E<bool>.V(true); }', 'type mismatch'),
        ('enum E<T> { V(T) } func main() { e := E<int>.V(1); x := e.(E<bool>.V); }', 'type arguments do not match'),
        ('enum E<T> { V(T) } func main() { e := E<int>.V(1); if (e == E<bool>.V) return; }', 'different enum specializations'),
        ('enum E<T> { V(T) } func main() { e := E<int>.V(1); match(e) { E<bool>.V(x) => return; } }', 'different enum type arguments'),
        ('enum E<T> { V(T) } func main() { e: E<any> = E<int>.V(1); }', 'type mismatch'),
        ('enum E<T> { V(T) } func main() { e := E<int>{0, 1}; }', 'qualified variant'),
        ('enum E<T ~int> { V(T) } func main() { e := E<bool>.V(true); }', 'constraint'),
    ]:
        rejected(body, message)

    for body in (
        'enum Result[T] { value(T) } func main() {}',
        'enum Result<T> { value(T) } func main() { r := Result[int].value(1); }',
        'enum Result<T> { value(T) } func f(r Result[int]) {} func main() {}',
        'enum Result<T> { value(T) } type Alias = Result[int]; func main() {}',
        'enum Result<T> { value(T) } func main() { r := Result<int>.value(1); v := r.(Result[int].value); }',
        'enum Result<T> { value(T) } func main() { r := Result<int>.value(1); match(r) { Result[int].value(v) => return; } }',
        'enum Result<T> { value(T) } type Alias = Result; func main() { r := Alias[int].value(1); }',
    ):
        rejected(body, 'require angle brackets')

    # Exercise the executable examples in the user's specification unchanged.
    fixtures = Path(__file__).with_name('fixtures')
    standard = root / 'standard'
    for name in ('io', 'reflect'):
        (standard / name).mkdir(parents=True)
        (standard / name / (name + '.oc')).write_text((fixtures / (name + '.oc')).read_text())
    environment['OCEAN_SRC'] = str(standard)
    spec = Path(__file__).resolve().parents[1].joinpath('testcode/enums.oc').read_text()
    spec = spec.removeprefix('package main;').split('\nfunc main() {', 1)[0]
    program(spec + '''
func main() {
    handleEvent(GuiEvent.GuiLoad);
    handleEvent(GuiEvent.KeyPress(65));
    handleEvent(GuiEvent.Click{x: 2, y: 3});
    matchEvent(GuiEvent.KeyPress(66));
    matchEvent(GuiEvent.Click{4, 5});
}''', output='GUI loaded.\nKeypress detected: 65\nClick at (2, 3)\nKey pressed: 66\nClick at (4, 5)\n')

    program('''import "reflect";
enum Event { A(int), B }
func main() int {
    e := Event.A(3); box: any = e;
    if (reflect.IsStruct(box) || len(reflect.Fields(box)) != 0) return 99;
    return 0;
}''')

    (root / 'ocean.mod').write_text('module enumtest\nocean 0.1\n')
    (root / 'lib').mkdir()
    (root / 'lib/lib.oc').write_text('''package lib;
enum Event { Load, Key(int), Click{x, y int} }
func Make() Event { return Event.Click{3, 4}; }
enum Result<T> { Value(T), Error(string) }
''')
    program('''import "lib";
x := 99;
func main() int {
    e := lib.Make();
    a, b := e.(lib.Event.Click{b = y, a = x});
    match(e) { lib.Event.Click{x, y} => return x + y + a + b;
               lib.Event => return 98; }
    return 99;
}''', 14)

    program('''import "lib";
func get<T>(r lib.Result<T>, fallback T) T {
    match(r) { lib.Result.Value(v) => return v; lib.Result => return fallback; }
    return fallback;
}
func main() int {
    a := lib.Result<int>.Value(5); n := a.(lib.Result<int>.Value);
    b := lib.Result<string>.Value("abc");
    return get<int>(a, 0) + len(get<string>(b, "")) + n;
}''', 13)

    for body, message in [
        ('enum E { A(int) } func main() { e := E.A(); }', 'argument count mismatch'),
        ('enum E { A(int) } func main() { e := E.A(true); }', 'type mismatch'),
        ('enum E { A } func main() { e := E.Missing; }', 'unknown enum variant'),
        ('enum E { A(int) } func main() { e := E.A; }', 'requires construction'),
        ('enum E { A{x int} } func main() { e := E.A{}; }', 'missing struct parameter initializer'),
        ('enum E { A{x int} } func main() { e := E.A{1}; p := e.(E.A); p.x = 2; }', 'immutable'),
        ('enum E { A } enum F { A } func main() { e: E = F.A; }', 'type mismatch'),
        ('enum E { A } enum F { A } func main() { e := E.A; if (e == F.A) return; }', 'different enum'),
        ('enum E { A(int) } enum F { A(int) } func main() { e := E.A(1); p := e.(F.A); }', 'same enum'),
        ('enum E { A(int, int) } func main() { e := E.A(1, 2); match(e) { E.A(x) => return; } }', 'pattern count mismatch'),
        ('enum E { A } func main() { e := E.A; match(e) { E.A => return; E.A => return; } }', 'duplicate enum match'),
        ('enum E { A{} } func main() { e := E.A{}; match(e) { E.A{x} => return; } }', 'unknown enum payload field'),
        ('enum E { A() } func main() { e := E.A(); match(e) { E.A(x) => return; } }', 'pattern count mismatch'),
        ('enum E { A(int) } func use(x E) {} func main() { use(E.A); }', 'requires construction'),
        ('enum E { A } func main() { e := E.A; match(e) { e => return; } }', 'must name the enum type'),
        ('enum E { A{x int} } func main() { n := 1; p := n.(E.A); }', 'same enum'),
        ('enum E { A{x int} } enum F { B } func main() { n := F.B; p := n.(E.A); }', 'same enum'),
        ('enum E { A } func main() { e := E.A; switch(v := e.(type)) { default: return; } }', 'type-guard switches do not support enums'),
    ]:
        rejected(body, message)

    program("""enum Result<T> { value(T), error(string), }
func (r Result<T>) unwrap() T {
    match(r) {
        Result.value(v) => return v;
        Result.error(msg) => return panic msg;
    }
    return zero(T);
}
func (r Result<T>) choose<U>(other U) U { return other; }
func (r *Result<T>) set(v T) { *r = Result<T>.value(v); }
enum Empty { none }
func (e Empty) number() int { return 2; }
func main() int {
    r := Result<int>.value(3);
    r.set(7);
    p := &r;
    if (p.unwrap() != 7) return 99;
    text := Result<string>.value("hello");
    if (text.unwrap() != "hello") return 98;
    return r.unwrap() + r.choose<int>(4) + Empty.none.number();
}""", 13)

    rejected("""enum Result { value(int), error(string) }
func (r Result) value() int { return 0; }
func main() {}""", "method name conflicts with an enum variant")

    rejected("""enum Result { value{item int}, empty }
func (r Result) item() int { return 0; }
func main() {}""", "method name conflicts with an enum payload field")

    rejected("""enum Result<T> { value(T), empty }
func (r Result<T>) unwrap() T { return zero(T); }
func main() {
    r := Result<int>.value(1);
    match(r) { Result.unwrap(v) => return; Result => return; }
}""", "unknown enum variant")

    rejected("""enum Result { value(int), empty }
func (r Result) unwrap() int { return 0; }
func main() {
    r := Result.value(1);
    match(r) { Result.unwrap => return; Result => return; }
}""", "unknown enum variant")

    rejected("""enum Result { value(int) }
func (r Result) unwrap() int { return 0; }
func (r Result) unwrap() int { return 1; }
func main() {}""", "duplicate method declaration")
