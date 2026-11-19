"""Unsigned parameterized widths, arithmetic, conversions, boxing and bitfields."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ocean-varuint-') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def check(body, message=None):
        source.write_text('package main;\n' + body)
        result = subprocess.run([compiler, 'build', '--check', str(source)], capture_output=True, text=True)
        assert (result.returncode == 0) == (message is None), result.stderr
        if message:
            assert message in result.stderr, result.stderr

    def program(body, status=0):
        check(body)
        result = subprocess.run([compiler, 'build', str(source), '--emit-llvm', '-o', str(ir)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        for opt in ('-O0', '-O2'):
            result = subprocess.run([clang, opt, '-Wno-override-module', str(ir), '-o', str(executable), '-lffi'], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            result = subprocess.run([str(executable)])
            assert result.returncode == status, (body, result.returncode)

    program('''type nibble varuint(4); type Alias = nibble;
func identity(x varuint(6)) varuint(6) { return x; }
func main() int {
    n := 15->nibble; n++; if (n->uint != 0) return 1;
    n += 9->Alias; if (n->uint != 9) return 2;
    x: varuint(6) = 63; wide: varuint(9) = x;
    signed: varint(7) = x; if (wide != 63 || signed != 63) return 3;
    if (x / 2 != 31 || x % 2 != 1 || x >> 1 != 31 || x <= 31) return 4;
    literal := varuint(4)[2]{8, 15}; dynamic := varuint(4)[]{9, 14};
    p := &x; *p = 61;
    b: any = x; y := b.(varuint(6));
    if (identity(y) != 61 || literal[1] != 15 || dynamic[0] != 9) return 5;
    f := x->float; if (f != 61.0 || f->varuint(6) != 61) return 6;
    bit := true->varuint(1); if (bit != 1 || !bit->bool) return 7;
    if (sizeof(varuint(4)) != 1 || sizeof(nibble) != 1 || zero(varuint(9)) != 0) return 8;
    return 0;
}''')
    body = []
    for width in range(1, 65):
        maximum = (1 << width) - 1
        body.append(f'x{width}: varuint({width}) = {maximum};')
        body.append(f'if (x{width}->uint != {maximum}) return 1;')
        body.append(f'b{width}: any = x{width}; y{width} := b{width}.(varuint({width}));')
        body.append(f'if (y{width}->uint != {maximum}) return 2;')
        body.append(f'x{width}++; if (x{width} != 0) return 3;')
        body.append(f'x{width}--; if (x{width}->uint != {maximum}) return 4;')
    program('func main() int { ' + ' '.join(body) + ' return 0; }')
    program('''type nibble varuint(4);
struct Flags { low varuint(4) : 4; high varuint(4) : 4; named nibble : 3; }
func main() int { b := Flags{15, 8, 7->nibble};
    if (b.low != 15 || b.high != 8 || b.named->uint != 7) return 1;
    b.low++; b.named++; if (b.low != 0 || b.high != 8 || b.named->uint != 0) return 2;
    b.high >>= 1; if (b.high != 4) return 3;
    return 0; }''')
    program('''extern i32 snprintf(buffer<i8>[] out, int size, buffer<i8>[] format, ...);
func main() int {
    out := i8[100]; a: varuint(6) = 63; b: varuint(32) = 4294967295;
    c: varuint(64) = 18446744073709551615;
    n := extern.snprintf(out.data, len(out), "%d %u %llu\\0".data, a, b, c);
    s := ""; for (i := 0; i < n; i++) s += out[i];
    if (s != "63 4294967295 18446744073709551615") return 1;
    return 0; }''')
    program('func main() int { x := (-1)->varuint(4); y: varuint(9) = x; if (x != 15 || y != 15 || (x->varint(4))->int != -1) return 1; return 0; }')
    check('struct B { x varuint(6) : 5; } func main() { x := B{31}; x.x++; }')
    result = subprocess.run([compiler, 'build', '--check', str(source)], capture_output=True, text=True)
    assert result.returncode == 0 and 'warning:' in result.stderr and 'smaller than explicit varuint(6)' in result.stderr, result.stderr
    check('type N varuint(6); type Alias = varuint(6); struct B { x N : 5; y Alias : 5; } func main() {}')
    result = subprocess.run([compiler, 'build', '--check', str(source)], capture_output=True, text=True)
    assert result.returncode == 0 and 'warning:' not in result.stderr, result.stderr
    for width in (0, 8388609, -1):
        check(f'func main() {{ x: varuint({width}); }}', 'varuint width')
    check('func main() { x: varuint; }', 'varuint requires')
    for body in (
        'func main() { x: varuint(4) = 16; }',
        'func main() { x: varuint(4) = -1; }',
        'func main() { x: varuint(6) = 3; y: varuint(4) = x; }',
        'func main() { x: varuint(6) = 3; y: varint(6) = x; }',
        'func main() { x: varint(6) = 3; y: varuint(6) = x; }',
        'type nibble varuint(4); func main() { x: nibble = 3; }',
        'func f(x varuint(4)[]) {} func main() { x: varint(4)[] = {}; f(x); }',
    ):
        check(body, 'type mismatch')
