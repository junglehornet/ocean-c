"""Verify zero/sizeof are IR constants before optimization and run them at -O0/-O2."""
import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


class Payload(ctypes.Structure):
    _fields_ = [("header", ctypes.c_bool), ("tag", ctypes.c_int8),
                ("value", ctypes.c_int64), ("name", ctypes.c_void_p),
                ("slots", ctypes.c_void_p * 4), ("child", ctypes.c_void_p)]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=15)


with tempfile.TemporaryDirectory(prefix="ocean constant builtins ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("constants.oc", "module.ll", "native"))
    source.write_text('''package main;
extern i32 puts(buffer<i8>[] text);
struct Plain { number int; flag bool; }
struct Payload<T> { header bool; tag i8; value T; name string;
                    slots buffer<string>[4]; child *Payload<T>; }
struct Inspector<T> { value T; }
func scalarZero() int { return zero(int); }
func plainZero() Plain { return zero(Plain); }
func aggregateZero() Payload<int> { return zero(Payload<int>); }
func bufferZero() buffer<string>[64] { return zero(buffer<string>[64]); }
func emptyZero() string { return zero(string); }
func intSize() int { return sizeof(int); }
func paddedSize() int { return sizeof(Payload<int>); }
func fixedSize() int { return sizeof(buffer<i8>[17]); }
func referenceSize() int { return sizeof(string); }
func valueSize() int { return sizeof(zero(buffer<string>[64])); }
func typeSize<T>() int { return sizeof(T); }
func defaultOf<T>() T { return zero(T); }
func (i Inspector<T>) bytes() int { return sizeof(T); }
func noisy() int { extern.puts("must not execute\\0".data); return 1; }
func main() int {
    p := aggregateZero(); b := bufferZero(); simple := plainZero();
    if (p.header || p.tag != 0 || p.value != 0 || p.name.size != 0 ||
        p.child != zero(*Payload<int>)) return 1;
    for (i := 0; i < 4; i++) { if (p.slots[i].size != 0) return 2; }
    for (i := 0; i < 64; i++) { if (b[i].size != 0) return 3; }
    if (simple.flag || simple.number != 0 || scalarZero() != 0 || emptyZero().size != 0) return 4;
    s := defaultOf<string>(); generic := defaultOf<Payload<int>>();
    if (s.size != 0 || generic.name.size != 0 || defaultOf<int>() != 0) return 5;
    inspector := zero(Inspector<Payload<int>>);
    n := 7;
    if (sizeof(++n) != sizeof(int) || n != 7 || sizeof(noisy()) != sizeof(int)) return 6;
    if (intSize() != INT_SIZE || paddedSize() != PAYLOAD_SIZE || fixedSize() != 17 ||
        referenceSize() != POINTER_SIZE || valueSize() != WIDE_SIZE ||
        typeSize<Payload<int>>() != PAYLOAD_SIZE || inspector.bytes() != PAYLOAD_SIZE) return 7;
    return 0;
}
'''.replace("INT_SIZE", str(ctypes.sizeof(ctypes.c_int64)))
       .replace("PAYLOAD_SIZE", str(ctypes.sizeof(Payload)))
       .replace("POINTER_SIZE", str(ctypes.sizeof(ctypes.c_void_p)))
       .replace("WIDE_SIZE", str(64 * ctypes.sizeof(ctypes.c_void_p))))
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    text = ir.read_text()
    bodies = dict(re.findall(r'^define[^\n]* @"([^"]+)"\([^\n]*\) \{\n(.*?)^}',
                             text, re.MULTILINE | re.DOTALL))
    zero_names = {"scalarZero", "plainZero", "aggregateZero", "bufferZero", "emptyZero"}
    size_names = {"intSize", "paddedSize", "fixedSize", "referenceSize", "valueSize"}
    assert len(bodies) >= len(zero_names) + len(size_names), text
    specializations = 0
    for name, body in bodies.items():
        # Function-wide box lifetime bookkeeping is independent of constant
        # evaluation. Remove only its exact initialization/cleanup sequence.
        region = re.search(r'  (%v\d+) = alloca ptr\n  store ptr null, ptr \1\n', body)
        if region:
            body = body.replace(region.group(0), "", 1)
            pointer = re.escape(region.group(1))
            body = re.sub(r'  %v\d+ = load ptr, ptr ' + pointer + r'\n'
                          r'  call void @__ocean\.any\.clean\(ptr ' + pointer + r', ptr null\)\n',
                          "", body)
        short = name.rsplit(".", 1)[-1].removeprefix("__pkg0_")
        if short in zero_names or short == "defaultOf":
            # Constants require no instructions to build them, even for long aggregates.
            lines = [line.strip() for line in body.splitlines() if line.strip() != "entry:"]
            if "malloc" not in body:
                assert len(lines) == 1 and lines[0].startswith("ret "), (name, body)
                assert not re.search(r'\b(insertvalue|alloca|load|call)\b', body), (name, body)
            else:
                # Owned return storage is materialized from a constant default.
                assert "ptr @.zero.array." in body, (name, body)
        if short in size_names or short in {"typeSize", "bytes"}:
            # Constant expressions retain target layout, rather than guessing padding.
            assert "ret i64 ptrtoint (ptr getelementptr (" in body, (name, body)
            # A by-value receiver may copy its owned arrays before the constant return.
            forbidden = r'= ptrtoint\b' if short == "bytes" else r'= (getelementptr|ptrtoint|load|call)\b'
            assert not re.search(forbidden, body), (name, body)
        if short in {"defaultOf", "typeSize", "bytes"}:
            specializations += 1
    assert specializations == 5, bodies.keys()
    assert "ret i64 0" in bodies["ocean.__pkg0_scalarZero"]
    assert "zeroinitializer" in bodies["ocean.__pkg0_plainZero"]
    assert "ptr @.zero.array." in bodies["ocean.__pkg0_aggregateZero"]
    assert len(bodies["ocean.__pkg0_bufferZero"]) > 128, "test must exercise untruncated aggregate constants"

    native = run([compiler, "build", str(source), "-o", str(executable)])
    assert native.returncode == 0, native.stdout + native.stderr
    result = run([str(executable)])
    assert (result.returncode, result.stdout) == (0, ""), result
    for optimization in ("-O0", "-O2"):
        built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable)])
        assert built.returncode == 0, built.stdout + built.stderr
        result = run([str(executable)])
        assert (result.returncode, result.stdout) == (0, ""), result

print("compile-time zero/sizeof tests passed in native, -O0, and -O2 builds")
