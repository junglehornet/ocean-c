"""Packed bitfield layout, value semantics, lvalues and diagnostics."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ocean-bitfields-') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def check(body, message=None, warning=None):
        source.write_text('package main;\n' + body)
        result = subprocess.run([compiler, 'build', '--check', str(source)], capture_output=True, text=True)
        assert (result.returncode == 0) == (message is None), result.stderr
        if message:
            assert message in result.stderr, result.stderr
        if warning:
            assert 'warning:' in result.stderr and warning in result.stderr, result.stderr
        elif message is None:
            assert 'warning:' not in result.stderr, result.stderr

    def program(body):
        check(body)
        result = subprocess.run([compiler, 'build', str(source), '--emit-llvm', '-o', str(ir)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        for opt in ('-O0', '-O2'):
            result = subprocess.run([clang, opt, '-Wno-override-module', str(ir), '-o', str(executable), '-lffi'], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            result = subprocess.run([str(executable)])
            assert result.returncode == 0, (body, result.returncode)

    program('''type nibble varint(4);
struct Bits { a, b uint : 4; c int : 3; flag bool : 1; value int; small nibble : 3; }
struct Byte { lo uint : 4; hi uint : 4; }
func take(b Bits) Bits { b.a = 12; return b; }
func main() int {
    if (sizeof(Byte) != 1) return 10;
    b := Bits{a: 15, b: 7, c: -2, flag: true, value: 33, small: 2->nibble};
    if (b.a != 15 || b.b != 7 || b.c != -2 || !b.flag || b.value != 33 || b.small->int != 2) return 1;
    b.a++; if (b.a != 0 || b.b != 7) return 2; b.c += 1;
    p := &b; p.b = 9; if (b.b != 9 || b.c != -1) return 3;
    b.flag = false; if (b.flag || b.c != -1) return 4;
    c := take(b); if (c.a != 12 || b.a != 0) return 5;
    a: Bits[2] = {b, c}; a[1].c = -4; if (a[1].b != 9 || a[0].c != -1) return 6;
    z: any = b; d := z.(Bits); if (d.b != 9 || d != b) return 7;
    b.a, b.b = b.b, b.a; if (b.a != 9 || b.b != 0) return 8;
    return 0;
}''')
    program('''struct Wide { x uint : 64; y int : 63; z uint : 2; }
struct Exact { x varint(6) : 6; y varint(1) : 1; }
func main() int {
    w := Wide{18446744073709551615, -4611686018427387904, 3};
    if (w.x != 18446744073709551615 || w.y != -4611686018427387904 || w.z != 3) return 1;
    w.x++; w.y--; w.z++; if (w.x != 0 || w.y != 4611686018427387903 || w.z != 0) return 2;
    e := Exact{-32, -1}; e.x++; if (e.x != -31 || e.y != -1) return 3;
    return 0;
}''')
    program('''struct Owned { a uint : 4; b uint : 4; text string; values int[]; c int : 2; }
func copy(x Owned) Owned { x.a = 2; x.values[0] = 9; return x; }
func main() int { x := Owned{a: 7, b: 12, c: -1};
    if (len(x.text) != 0 || len(x.values) != 0 || x.a != 7) return 1;
    x.text = "abc"; x.values += 3;
    y := copy(x); if (y.a != 2 || y.b != 12 || y.c != -1 || y.text != "abc" || y.values[0] != 9 || x.values[0] != 3) return 2;
    z: any = y; back := z.(Owned); if (back != y) return 3;
    return 0; }''')
    program('''struct Generic<T> { x T : 3; y uint : 5; }
func main() int { x := Generic<int>{-2, 17}; x.x++; if (x.x != -1 || x.y != 17 || sizeof(Generic<int>) != 1) return 1; return 0; }''')
    fields, checks = [], []
    for width in range(1, 65):
        fields.append(f's{width} int : {width}; u{width} uint : {width};')
        minimum, maximum = -(1 << (width - 1)), (1 << width) - 1
        checks.append(f'x.s{width} = {minimum}; x.u{width} = {maximum};')
        checks.append(f'if (x.s{width} != {minimum} || x.u{width} != {maximum}) return 1;')
        checks.append(f'x.s{width}--; x.u{width}++;')
        checks.append(f'if (x.s{width} != {(1 << (width - 1)) - 1} || x.u{width} != 0) return 2;')
    program('struct All { ' + ' '.join(fields) + ' } func main() int { x: All; ' + ' '.join(checks) + ' return 0; }')
    check('struct B { num varint(6) : 5; } func main() { b := B{15}; b.num++; }', warning='smaller than explicit varint(6)')
    check('type Small varint(6); type Alias = Small; type Raw = varint(6); struct B { a Small : 5; b Alias : 5; c Raw : 5; } func main() {}')
    for field, message in (
        ('a float : 3;', 'integer or bool'),
        ('a string : 3;', 'integer or bool'),
        ('a bool : 2;', 'exceeds its type width'),
        ('a varint(4) : 5;', 'exceeds its type width'),
        ('a i8 : 9;', 'exceeds its type width'),
        ('a int : 0;', 'bitfield size'),
        ('a int : 65;', 'bitfield size'),
        ('a int : -1;', 'bitfield size'),
        ('a int : n;', 'bitfield size'),
    ):
        check(f'struct B {{ {field} }} func main() {{}}', message)
    check('struct B { a uint : 4; } func main() { b: B; p := &b.a; }', 'address of a bitfield')
    check('struct B<T> { a T : 4; } func main() { b: B<float>; }', 'integer or bool')
    check('struct B<T> { a T : 9; } func main() { b: B<i8>; }', 'exceeds its type width')
    check('type N int; func (n *N) Set() { *n = 1->N; } struct B { a N : 4; } func main() { b: B; b.a.Set(); }', 'address of a bitfield')
