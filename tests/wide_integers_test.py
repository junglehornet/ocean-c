"""Arbitrary-width signed and unsigned integers, exact literals and boxed storage."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ocean-wide-') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def run(*args):
        return subprocess.run(args, capture_output=True, text=True, timeout=90)

    def emit(body):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert result.returncode == 0, result.stderr
        return ir.read_text()

    def program(body):
        emit(body)
        for opt in ('-O0', '-O2'):
            result = run(clang, opt, '-Wno-override-module', str(ir), '-o', str(executable), '-lffi')
            assert result.returncode == 0, result.stderr
            result = run(str(executable))
            assert result.returncode == 0 and result.stderr == '', (body, result)

    def rejected(body, message):
        source.write_text('package main;\n' + body)
        result = run(compiler, 'build', '--check', str(source))
        assert result.returncode != 0 and message in result.stderr, result

    for width in (65, 66, 80, 127, 128, 129, 192, 256, 257, 511, 512, 1024):
        maximum, minimum = (1 << width) - 1, -(1 << (width - 1))
        program(f'''func id(x varuint({width})) varuint({width}) {{ return x; }}
func main() int {{
    x: varuint({width}) = {maximum}; y: varint({width}) = {minimum};
    if (x != {maximum} || y != {minimum}) return 1;
    x++; y--; if (x != 0 || y != {(1 << (width - 1)) - 1}) return 2;
    x--; y++; if (x != {maximum} || y != {minimum}) return 3;
    b: any = id(x); z := b.(varuint({width})); if (z != x) return 4;
    c: any = y; d := c.(varint({width})); if (d != y) return 5;
    return 0;
}}''')

    program('''func main() int {
    x: varint(128) = -9223372036854775809;
    y: varint(65) = -18446744073709551615;
    if (x != -9223372036854775809 || y != -18446744073709551615) return 1;
    if (x->varuint(128) != 340282366920938463454151235394913435647) return 2;
    z := -(-18446744073709551615); b: any = z;
    if (b.(uint) != 18446744073709551615) return 3;
    return 0;
}''')

    program('''type Big varuint(256);
func divide(x varuint(256), y varuint(256)) (varuint(256), varuint(256)) { return x / y, x % y; }
func signedDivide(x varint(256), y varint(256)) varint(256) { return x / y; }
func main() int {
    x: varuint(256) = 340282366920938463463374607431768211456;
    y: varuint(256) = 0x100000000000000000000000000000001;
    binary: varuint(128) = 0b10000000000000000000000000000000000000000000000000000000000000000;
    octal: varuint(128) = 0o2000000000000000000000;
    if (binary != octal || x + 1 != y || (x >> 128) != 1) return 1;
    q, r := divide(y, 3); if (q != 113427455640312821154458202477256070485 || r != 2) return 2;
    if (signedDivide(-340282366920938463463374607431768211456, 2) != -170141183460469231731687303715884105728) return 3;
    n := y->Big; box: any = n; back := box.(Big); if (back != n) return 4;
    narrow := y->varuint(128); if (narrow != 1) return 5;
    small: varuint(65) = 18446744073709551616; large: varuint(256) = small;
    neg: varint(65) = -18446744073709551616; signed: varint(256) = neg;
    if (large != 18446744073709551616 || signed != -18446744073709551616) return 6;
    if ((large->float) != 18446744073709551616.0) return 7;
    return 0;
}''')

    program('''func identity<T any>(x T) T { return x; }
func add<T ~varuint(256)>(a T, b T) T { return a + b; }
func main() int {
    a: varuint(128) = 340282366920938463463374607431768211455;
    b: varuint(256) = 1; c := a + b;
    if (sizeof(c) != sizeof(varuint(256)) || c != 340282366920938463463374607431768211456) return 1;
    s: varint(128) = -1; combined := a + s;
    if (combined != 340282366920938463463374607431768211454 || sizeof(combined) != sizeof(varint(129))) return 2;
    if (add<varuint(256)>(identity<varuint(256)>(c), b) != 340282366920938463463374607431768211457) return 3;
    return 0;
}''')

    program('''struct Record { value varuint(256); signed varint(129); }
struct Bits { a varuint(128) : 4; b varint(256) : 5; }
func copy(x Record) Record { return x; }
func main() int {
    x := Record{340282366920938463463374607431768211456, -18446744073709551616};
    a: Record[2] = {x, copy(x)}; a[1].value++;
    if (a[0].value + 1 != a[1].value) return 1;
    dynamic: varuint(256)[] = {x.value}; dynamic += x.value + 1;
    if (dynamic[1] != a[1].value) return 2;
    box: any = x; y := box.(Record); if (y != x) return 3;
    boxes: any[] = {x.value, y.value}; if (boxes[0] != boxes[1]) return 4;
    m: map[varuint(256)]int = {}; m[x.value] = 9;
    if (m[y.value] != 9) return 5;
    b := Bits{15, -16}; b.a++; b.b++; if (b.a != 0 || b.b != -15) return 6;
    p := new(varuint(256)); *p = dynamic[1]; if (*p != dynamic[1]) return 7;
    return 0;
}''')

    program('''type Count varuint(256);
func (c Count) Get() Count { return c; }
interface Getter { Get() Count; }
func closure(x varuint(256)) func() varuint(256) { return func() varuint(256) { x++; return x; }; }
func main() int {
    n := 340282366920938463463374607431768211456->Count;
    g: Getter = n; if (g.Get() != n) return 1;
    next := closure(n->varuint(256)); if (next() != 340282366920938463463374607431768211457) return 2;
    if (next() != 340282366920938463463374607431768211458) return 3;
    return 0;
}''')

    for family in ('varint', 'varuint'):
        text = emit(f'func accept(x {family}(8388608)) {{}} func main() int {{ accept(zero({family}(8388608))); return 0; }}')
        assert 'i8388608' in text
        result = run(clang, '-Wno-override-module', '-S', '-emit-llvm', '-x', 'ir', str(ir), '-o', str(root / 'verified.ll'))
        assert result.returncode == 0, result.stderr
        for width in (0, 8388609, -1, 18446744073709551617):
            rejected(f'func main() {{ x: {family}({width}); }}', 'width')
    rejected('func main() { x: varint(128) = 170141183460469231731687303715884105728; }', 'out of range')
    rejected('func main() { x: varint(128) = -170141183460469231731687303715884105729; }', 'out of range')
    rejected('func main() { x: varuint(128) = 340282366920938463463374607431768211456; }', 'out of range')
    rejected('func main() { x: varuint(128) = -1; }', 'out of range')
    rejected('func main() { x: varuint(256) = 1; y: varuint(128) = x; }', 'type mismatch')
    rejected('extern int f(int x, ...); func main() { x: varuint(128) = 1; extern.f(1, x); }', 'C interoperability')
