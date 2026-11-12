"""Compiler-owned Result<T>, managed unwraps, integer bounds and decimal rounding."""
from pathlib import Path
import decimal
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixtures = Path(__file__).with_name('fixtures')


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, timeout=60, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean result ') as directory:
    root = Path(directory)
    standard = root / 'standard'
    for target, fixture in (('io/io.oc', 'io.oc'), ('io/parsing.oc', 'io_parsing.oc'),
                            ('reflect/reflect.oc', 'reflect.oc')):
        path = standard / target
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text((fixtures / fixture).read_text())
    env = dict(os.environ, OCEAN_SRC=str(standard))
    app = root / 'program'
    app.mkdir()
    source, ir, executable = app / 'main.oc', root / 'main.ll', root / 'app'

    def program(body, error=None):
        source.write_text('package main;\n' + body)
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        for opt in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', opt, str(ir), '-o', str(executable), '-lffi'])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)])
            if error:
                assert result.returncode != 0 and error in result.stderr, result
                assert 'main.oc:' in result.stderr, result.stderr
            else:
                assert result.returncode == 0, (opt, result)

    # No import is needed, and generic payloads retain ordinary ownership rules.
    program('''struct S { text string; values int[2]; }
func forward<T>(value Result<T>, fallback T) T { return value.unwrapOrElse(fallback); }
func main() int {
    value := Result<int>.value(7); error := Result<int>.error("bad");
    if (value.unwrap() != 7 || value.unwrapOrElse(3) != 7 || error.unwrapOrElse(3) != 3 || error.unwrapOrZero() != 0) return 1;
    if (forward<int>(error, 9) != 9) return 2;
    empty := Result<string>.error("empty").unwrapOrZero();
    if (empty != "" || Result<bool>.error("bad").unwrapOrZero()) return 3;
    p := Result<*int>.error("bad").unwrapOrZero();
    if (p != zero(*int)) return 4;
    record := Result<S>.error("bad").unwrapOrZero();
    if (record.text != "" || record.values[0] != 0) return 5;
    values := Result<int[]>.value(int[]{1, 2}).unwrap(); values[0] = 9;
    result := Result<S>.value(S{"owned", {3, 4}});
    a := result.unwrap(); a.text = "changed"; a.values[0] = 99;
    b := result.unwrap();
    if (b.text != "owned" || b.values[0] != 3 || values[0] != 9) return 6;
    for (i := 0; i < 100; i++) {
        text := Result<string>.value("stable").unwrap();
        fallback := Result<string>.error("bad").unwrapOrElse(text);
        if (fallback != "stable") return 7;
    }
    match(value) { Result<int>.value(n) => { if (n != 7) return 8; }
                   Result<int>.error(message) => return 9; }
    return 0;
}''')
    program('''func create() Result<int> { return Result<int>.value(8); }
func shadow() int { type Result = int; return create().unwrap(); }
func main() int { if (shadow() != 8) return 1; return 0; }''')
    program('func main() { Result<int>.error("bad number").unwrap(); }', error='Panic: bad number')

    # Built-in inspection methods distinguish tags, including a present null.
    program('''func inspect<T>(result Result<T>) bool { return result.hasError(); }
func present<T>(optional Optional<T>) bool { return optional.hasValue(); }
func main() int {
    success := Result<int>.value(42);
    failure := Result<int>.error("stored error");
    if (success.hasError() || !failure.hasError() || !inspect<string>(Result<string>.error(""))) return 1;
    if (failure.error() != "stored error" || Result<string>.error("").error() != "") return 2;
    message := failure.error(); message = "changed";
    if (failure.error() != "stored error") return 3;
    absent: Optional<int>;
    if (absent.hasValue() || zero(Optional<int>).hasValue() || Optional<int>.None.hasValue()) return 4;
    if (!Optional<int>.Some(0).hasValue() || !present<string>(Optional<string>.Some(""))) return 5;
    if (!Optional<*int>.Some(null).hasValue()) return 6;
    return 0;
}''')
    program('func main() { Result<int>.value(1).error(); }',
            error='Panic: Result.error() called on a value result')

    program('''struct Payload { text string; values int[2]; }
func extract<T>(value Optional<T>) T { return value.unwrap(); }
func main() int {
    if (Optional<int>.Some(42).unwrap() != 42) return 1;
    if (extract<string>(Optional<string>.Some("text")) != "text") return 2;
    if (Optional<*int>.Some(null).unwrap() != null) return 3;
    original := Optional<Payload>.Some(Payload{"owned", {3, 4}});
    copy := original.unwrap(); copy.text = "changed"; copy.values[0] = 99;
    next := original.unwrap();
    if (next.text != "owned" || next.values[0] != 3) return 4;
    return 0;
}''')
    program('func main() { Optional<int>.None.unwrap(); }',
            error='Panic: Optional.unwrap() called on None')
    program('func main() { value: Optional<string>; value.unwrap(); }',
            error='Panic: Optional.unwrap() called on None')

    # Parsing returns errors rather than panicking, until unwrap is requested.
    valid_ints = ['0', '-0', '+0', '7', '-42', '+123', '000123',
                  '9223372036854775807', '-9223372036854775808']
    invalid_ints = ['', '+', '-', ' 1', '1 ', '1.0', '0x10', '1_000', '1\0',
                    '9223372036854775808', '-9223372036854775809', '9' * 1000]
    text = 'import "io"; func main() int {\n'
    for value in valid_ints:
        expected = int(value)
        expression = '(-9223372036854775807 - 1)' if expected == -(1 << 63) else str(expected)
        text += f'if (io.ParseInt({json.dumps(value)}).unwrap() != {expression}) return 1;\n'
    for value in invalid_ints:
        # JSON's \u0000 is a supported Ocean Unicode escape for a NUL byte.
        text += f'if (io.ParseInt({json.dumps(value)}) != Result<int>.error) return 2;\n'
    text += 'if (io.ParseInt("bad").unwrapOrElse(23) != 23 || io.ParseInt("bad").unwrapOrZero() != 0) return 3;\nreturn 0; }'
    program(text)

    rng = random.Random(48)
    numbers = ['0', '-0', '+0', '.5', '1.', '0.1', '-1.25e2', '1e308',
               '1.7976931348623157e308', '5e-324', '2.4703282292062327e-324',
               '2.4703282292062328e-324', '2.2250738585072014e-308',
               '1e-99999', '-1e-99999', '0e' + '9' * 1000]
    for _ in range(80):
        bits = rng.getrandbits(64)
        value = struct.unpack('=d', struct.pack('=Q', bits))[0]
        if math.isfinite(value): numbers.append(repr(value))
    decimal.getcontext().prec = 2000
    for bits in (0, 1, 2, 0x3fefffffffffffff, 0x3ff0000000000000, 0x3ff0000000000001, 0x0010000000000000):
        lower = struct.unpack('=d', struct.pack('=Q', bits))[0]
        upper = struct.unpack('=d', struct.pack('=Q', bits + 1))[0]
        midpoint = (decimal.Decimal.from_float(lower) + decimal.Decimal.from_float(upper)) / 2
        numbers.append(str(midpoint))
    text = '''import "io";
func bitsOf(x float) uint { raw: *void = &x; return *(raw -> (*uint)); }
func main() int {\n'''
    for index, value in enumerate(numbers):
        expected = struct.unpack('=Q', struct.pack('=d', float(value)))[0]
        text += f'if (bitsOf(io.ParseFloat({json.dumps(value)}).unwrap()) != {expected}->uint) panic "float case {index}";\n'
    for value in ('', '+', '-', '.', 'e1', '1e', '1e+', '1.2.3', ' 1', '1 ', '1_0', '0x1p2',
                  '1\0', '1e309', '-1e99999', '1.7976931348623159e308'):
        text += f'if (io.ParseFloat({json.dumps(value)}) != Result<float>.error) return 1;\n'
    text += '''nan := io.ParseFloat("NaN").unwrap();
if (nan == nan) return 2;
if (bitsOf(io.ParseFloat("+INF").unwrap()) != 9218868437227405312->uint) return 3;
if (bitsOf(io.ParseFloat("-infinity").unwrap()) != 18442240474082181120->uint) return 4;
if (io.ParseFloat("bad").unwrapOrElse(1.25) != 1.25 || io.ParseFloat("bad").unwrapOrZero() != 0.0) return 5;
return 0; }'''
    program(text)
    program('import "io"; func main() { io.ParseInt("no").unwrap(); }', error='invalid integer')

    for body in ('func main() { Result<int>.value(1).unwrap(2); }',
                 'func main() { Result<int>.value(1).unwrapOrElse(); }',
                 'func main() { Result<int>.error(1); }',
                 'func main() { Result<int>.value(1).hasError(1); }',
                 'func main() { Result<int>.error("bad").error(1); }',
                 'func main() { Optional<int>.Some(1).hasValue(1); }',
                 'func main() { Optional<int>.Some(1).unwrap(2); }',
                 'enum E { error(string) } func (e E) error() string { return ""; } func main() {}'):
        source.write_text('package main; ' + body)
        result = run([compiler, 'check', str(source)], env=env)
        assert result.returncode != 0, body

print('Result ownership, panic and numeric parsing tests passed')
