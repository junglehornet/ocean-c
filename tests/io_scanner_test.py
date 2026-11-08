"""Buffered scans, nonblocking readiness, file modes and shared close state."""
from pathlib import Path
import json
import os
import platform
import re
import selectors
import subprocess
import sys
import tempfile

compiler, clang, probe = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixtures = Path(__file__).with_name('fixtures')


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, timeout=40, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean scanner ') as directory:
    root = Path(directory)
    standard = root / 'standard'
    for target, fixture in (
        ('io/io.oc', 'io.oc'), ('io/scanner.oc', 'io_scanner.oc'),
        ('os/args.oc', 'os_args.oc'), ('os/streams.oc', 'os_streams.oc'),
        ('os/files.oc', 'os_files.oc'), ('os/reads.oc', 'os_reads.oc'),
        ('os/seek.oc', 'os_seek.oc'), ('reflect/reflect.oc', 'reflect.oc'),
    ):
        destination = standard / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text((fixtures / fixture).read_text())
    environment = dict(os.environ, OCEAN_SRC=str(standard))
    app = root / 'program'
    app.mkdir()
    source = app / 'main.oc'
    ir, executable = root / 'main.ll', root / 'main'

    def build(body):
        source.write_text('package main; import "io"; import "os";\n' + body)
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)],
                     env=environment, text=True)
        assert result.returncode == 0, result.stdout + result.stderr

    link_extra = []

    def native(opt):
        result = run([clang, '-Wno-override-module', opt, str(ir), '-o', str(executable), '-lffi', *link_extra], text=True)
        assert result.returncode == 0, result.stderr

    def program(body, data=b'', output=b'ok\n', error=None, prepare=None):
        build(body)
        for opt in ('-O0', '-O2'):
            native(opt)
            if prepare:
                prepare()
            result = run([str(executable)], input=data)
            if error:
                assert result.returncode != 0 and error.encode() in result.stderr, (opt, result)
            else:
                assert result.returncode == 0 and result.stdout == output, (opt, result)

    program('''func main() {
        scanner := io.NewScanner(os.Stdin().unwrap());
        if (scanner.Scan().unwrap() != "" || scanner.Scan().unwrap() != "alpha") panic "leading delimiters";
        if (scanner.Scanln().unwrap() != "beta") panic "shared buffer";
        if (scanner.Scanln().unwrap() != "line with spaces\\r") panic "line bytes";
        if (scanner.Scanc(',').unwrap() != "x" || scanner.Scanc(',').unwrap() != "") panic "delimiter";
        if (scanner.Scanln().unwrap() != "tail") panic "line";
        readOutcome1 := scanner.Scann(3).unwrap(); bytes := readOutcome1.Bytes; end := readOutcome1.End;
        if (bytes -> string != "XYZ" || end) panic "exact count";
        readOutcome2 := scanner.Scann(1).unwrap(); missing := readOutcome2.Bytes; short := readOutcome2.End;
        if (len(missing) != 0 || !short) panic "short EOF";
        readOutcome3 := scanner.Scann(0).unwrap(); empty := readOutcome3.Bytes; zeroEnd := readOutcome3.End;
        if (len(empty) != 0 || zeroEnd) panic "zero count";
        if (scanner.HasNext().unwrap() || scanner.HasNextln().unwrap() || scanner.HasNextc(',').unwrap()) panic "empty EOF";
        if (scanner.Scan().unwrap() != "" || scanner.Scanln().unwrap() != "") panic "repeated EOF";
        io.Println("ok");
    }''', b' alpha\tbeta\nline with spaces\r\nx,,tail\nXYZ')

    program('''func main() {
        if (io.Scan().unwrap() != "a" || io.Scanln().unwrap() != " b") panic "convenience buffer";
        if (io.Scan().unwrap() != "next" || io.Scanln().unwrap() != "word") panic "convenience order";
        if (io.Scanln().unwrap() != "") panic "convenience EOF";
        io.Println("ok");
    }''', b'a  b\nnext word\n')

    program('''func main() {
        scanner := io.NewScanner(os.Stdin().unwrap());
        for (i := 0; i < 6; i++) if (scanner.Scan().unwrap() != "") panic "ASCII whitespace";
        if (scanner.Scanc('\\0').unwrap() != "one") panic "NUL delimiter";
        readOutcome4 := scanner.Scann(9).unwrap(); bytes := readOutcome4.Bytes; end := readOutcome4.End;
        if (bytes -> string != "two" || !end) panic "short final chunk";
        io.Println("ok");
    }''', b'\t\n\v\f\r one\0two')

    program('''func main() {
        scanner := io.NewScanner(os.Stdin().unwrap());
        if (len(scanner.Scanln().unwrap()) != 3000) panic "first long line";
        if (!scanner.HasNextln().unwrap() || !scanner.HasNextln().unwrap()) panic "read-ahead";
        if (len(scanner.Scanln().unwrap()) != 6000) panic "compaction";
        if (!scanner.HasNextln().unwrap() || scanner.Scanln().unwrap() != "last") panic "final line";
        if (scanner.HasNextln().unwrap()) panic "final EOF";
        io.Println("ok");
    }''', b'p' * 3000 + b'\n' + b'x' * 6000 + b'\nlast')

    path = root / 'résumé-🌊.txt'
    quoted = json.dumps(str(path), ensure_ascii=False)
    program('''func main() {
        if (os.Stdout().unwrap().CanRead() || os.Stderr().unwrap().CanRead() || !os.Stdin().unwrap().CanRead()) panic "stream permissions";
        if (!os.Stdout().unwrap().CanWrite() || !os.Stderr().unwrap().CanWrite() || os.Stdin().unwrap().CanWrite()) panic "stream modes";
        file := os.Open(PATH, os.FileMode.Write).unwrap();
        if (file.Write("abc").unwrap() != 3) panic "write";
        copy := file;
        file.Close(); copy.Close();
        if (copy.IsOpen() || copy.CanRead() || copy.CanWrite()) panic "shared close";
        file = os.Open(PATH, os.FileMode.ReadWrite).unwrap();
        if (file.Read().unwrap() != 97) panic "readwrite preserves";
        if (file.Write("Z").unwrap() != 1) panic "readwrite offset";
        copy.Close(); // A stale close must not close a reused descriptor/registry slot.
        if (!file.IsOpen()) panic "stale generation";
        file.Close();
        file = os.Open(PATH, os.FileMode.Append).unwrap();
        file.Write("d").unwrap(); file.Close();
        file = os.Open(PATH, os.FileMode.ReadAppend).unwrap();
        if (file.Read().unwrap() != 97) panic "readappend read";
        file.Write("e").unwrap(); file.Close();
        file = os.Open(PATH, os.FileMode.Read).unwrap();
        scanner := io.NewScanner(file);
        readOutcome5 := scanner.Scann(5).unwrap(); bytes := readOutcome5.Bytes; end := readOutcome5.End;
        if (bytes -> string != "aZcde" || end) panic "append location";
        file.Close();
        file = os.Open(PATH, os.FileMode.Write).unwrap();
        file.Write("a\\0b").unwrap(); file.Close();
        file = os.Open(PATH, os.FileMode.Read).unwrap();
        if (len(file.Readchunk(0).unwrap()) != 0 || file.Readchunk(9).unwrap() -> string != "a\\0b") panic "truncate or binary";
        file.Close();
        io.Println("ok");
    }'''.replace('PATH', quoted))
    assert path.read_bytes() == b'a\0b'

    byte_program = '''func sized(bytes array<i8>) int { return len(bytes); }
func main() {
    file := os.Open(PATH, os.FileMode.Binary).unwrap();
    if (!file.IsBinary() || file.Size().unwrap() != 4) panic "binary size";
    if (file.Read().unwrap() != 0) panic "first byte";
    all := file.Readall().unwrap();
    if (sized(all) != 4 || all[0] != 0 || (all[3]->u8)->int != 255) panic "whole static array";
    if (file.Read().unwrap() != 127) panic "Readall moved cursor";
    if (file.Size().unwrap() != 4) panic "size";
    readOutcome6 := file.Readn(4).unwrap(); tail := readOutcome6.Bytes; early := readOutcome6.End;
    if (sized(tail) != 4 || !early || (tail[0]->u8)->int != 128 ||
        (tail[1]->u8)->int != 255 || tail[2] != 0 || tail[3] != 0) panic "zero padded read";
    file.Setpos(0);
    readOutcome7 := file.Readn(4).unwrap(); exact := readOutcome7.Bytes; short := readOutcome7.End;
    if (len(exact) != 4 || short || file.Read().unwrap() != -1) panic "exact EOF";
    file.Setpos(1);
    ending := file.Readend().unwrap();
    if (sized(ending) != 3 || ending[0] != 127 || (ending[2]->u8)->int != 255) panic "Readend";
    empty := file.Readend().unwrap();
    readOutcome8 := file.Readn(0).unwrap(); zero := readOutcome8.Bytes; zeroEnd := readOutcome8.End;
    if (len(empty) != 0 || len(zero) != 0 || zeroEnd) panic "empty reads";
    file.Setpos(2);
    if (file.Read().unwrap() != 128 || file.Read().unwrap() != 255 || file.Read().unwrap() != -1) panic "unsigned byte or EOF";
    file.Setpos(file.Size().unwrap());
    if (file.Read().unwrap() != -1) panic "seek EOF";
    file.Close();
    if ((all[3]->u8)->int != 255 || (tail[1]->u8)->int != 255) panic "owned static data";
    file = os.Open(PATH, os.FileMode.BinaryWrite).unwrap();
    if (!file.IsBinary() || file.Size().unwrap() != 0) panic "binary write";
    file.Write("ab").unwrap(); file.Close();
    file = os.Open(PATH, os.FileMode.BinaryAppend).unwrap();
    if (!file.IsBinary() || file.Size().unwrap() != 2) panic "binary append";
    file.Write("c").unwrap(); file.Close();
    file = os.Open(PATH, os.FileMode.BinaryReadWrite).unwrap();
    if (!file.IsBinary() || file.Read().unwrap() != 97) panic "binary readwrite";
    file.Setpos(1); file.Write("Z").unwrap(); file.Close();
    file = os.Open(PATH, os.FileMode.BinaryReadAppend).unwrap();
    file.Setpos(1); file.Write("d").unwrap(); file.Close();
    file = os.Open(PATH, os.FileMode.Read).unwrap();
    if (file.IsBinary() || file.Size().unwrap() != 4) panic "text mode";
    text := file.Readall().unwrap();
    if (text[0] != 97 || text[1] != 90 || text[2] != 99 || text[3] != 100) panic "binary mode flags";
    if (file.Read().unwrap() != 97) panic "text Read";
    file.Close();
    io.Println("ok");
}
'''
    program(byte_program.replace('PATH', quoted),
            prepare=lambda: path.write_bytes(bytes((0, 127, 128, 255))))

    program('''func main() {
        file := os.Open(PATH, os.FileMode.Write).unwrap();
        file.Write("alpha beta\\nlast").unwrap(); file.Close();
        file = os.Open(PATH, os.FileMode.Read).unwrap();
        scanner := io.NewScanner(file);
        if (scanner.Scan().unwrap() != "alpha") panic "buffer prime";
        file.Setpos(0);
        if (scanner.Scanln().unwrap() != "alpha beta") panic "seek invalidates buffer";
        whole := file.Readall().unwrap();
        if (len(whole) != 15 || scanner.Scanln().unwrap() != "last") panic "Readall preserves scanner";
        if (scanner.HasNextln().unwrap()) panic "scanner EOF";
        file.Setpos(0);
        if (!scanner.HasNext().unwrap() || scanner.Scan().unwrap() != "alpha") panic "seek restarts scanner";
        file.Close();
        file = os.Open(PATH, os.FileMode.Write).unwrap(); file.Close();
        file = os.Open(PATH, os.FileMode.Binary).unwrap();
        if (file.Size().unwrap() != 0 || len(file.Readall().unwrap()) != 0) panic "empty whole file";
        file.Setpos(0);
        if (file.Read().unwrap() != -1) panic "empty EOF";
        file.Close();
        io.Println("ok");
    }'''.replace('PATH', quoted))

    for body, message in (
        ('io.NewScanner(os.Stdout().unwrap());', 'readable stream'),
        ('io.NewScanner(os.Stderr().unwrap());', 'readable stream'),
        ('s := io.NewScanner(os.Stdin().unwrap()); s.Scann(-1).unwrap();', 'invalid scan size'),
        ('f := os.Open(PATH, os.FileMode.Binary).unwrap(); io.NewScanner(f);', 'binary file'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Readn(-1).unwrap();', 'invalid read size'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(-1);', 'position out of bounds'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(f.Size().unwrap() + 1);', 'position out of bounds'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Readend().unwrap(); f.Read().unwrap();', 'read past EOF'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(f.Size().unwrap()); f.Read().unwrap(); f.Read().unwrap();', 'read past EOF'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(f.Size().unwrap()); f.Read().unwrap(); f.Readall().unwrap(); f.Read().unwrap();', 'read past EOF'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); copy := f; f.Readend().unwrap(); copy.Read().unwrap();', 'read past EOF'),
        ('os.Open(' + json.dumps(str(root / 'missing')) + ', os.FileMode.Read).unwrap();', 'could not open file'),
        ('os.Open("bad\\0path", os.FileMode.Read).unwrap();', 'contains NUL'),
        ('f := os.Open(PATH, os.FileMode.Write).unwrap(); f.Read().unwrap();', 'not readable'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Write("x").unwrap();', 'not writable'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); s := io.NewScanner(f); s.HasNext().unwrap(); f.Close(); s.Scan().unwrap();', 'source is closed'),
    ):
        program('func main() { ' + body.replace('PATH', quoted) + ' }', error=message)

    # The same functions used by live scanning must be accepted on every target.
    targets = {
        'x86_64/linux': 'x86_64-linux-gnu', 'aarch64/linux': 'aarch64-linux-gnu',
        'riscv64/linux': 'riscv64-linux-gnu', 'x86_64/macos': 'x86_64-apple-macosx11.0',
        'aarch64/macos': 'arm64-apple-macosx11.0', 'x86_64/windows': 'x86_64-windows-msvc',
        'aarch64/windows': 'aarch64-windows-msvc',
    }
    foreign = root / 'foreign.oc'
    for fixture in ('os_streams.oc', 'os_files.oc', 'os_seek.oc'):
        text = (fixtures / fixture).read_text()
        assert not re.search(r'\bextern\b', text)
        for match in re.finditer(r'asm (\w+/\w+) \{(.*?)^\s*\}', text, re.S | re.M):
            selector, assembly = match.groups()
            foreign.write_text('package probe; func sample() { asm ' + selector + ' {' + assembly + '\n} }')
            result = run([probe, str(foreign)], text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            ir.write_text(result.stdout)
            for opt in ('-O0', '-O2'):
                result = run([clang, '--target=' + targets[selector], '-Wno-override-module', opt,
                              '-c', '-x', 'ir', str(ir), '-o', str(root / 'foreign.o')], text=True)
                assert result.returncode == 0, (selector, opt, result.stderr)

    if os.name == 'posix':
        read_control, write_control = os.pipe()
        build('''func main() {
            scanner := io.NewScanner(os.Stdin().unwrap());
            control := os.FromDescriptor(CONTROL, true, false);
            for (;;) {
                command := control.Readchunk(1).unwrap();
                if (len(command) == 0) break;
                switch(command[0]) {
                    case 'p': io.Println(scanner.HasNext().unwrap(), " ", scanner.HasNextln().unwrap(), " ", scanner.HasNextc(',').unwrap());
                    case 's': io.Println("[", scanner.Scan().unwrap(), "]");
                    case 'l': io.Println("[", scanner.Scanln().unwrap(), "]");
                    case 'c': io.Println("[", scanner.Scanc(',').unwrap(), "]");
                }
            }
            control.Close();
        }'''.replace('CONTROL', str(read_control)))
        for opt in ('-O0', '-O2'):
            native(opt)
            child = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, pass_fds=(read_control,))
            selector = selectors.DefaultSelector()
            selector.register(child.stdout, selectors.EVENT_READ)

            def command(letter, expected):
                os.write(write_control, letter)
                assert selector.select(3), 'HasNext blocked waiting for stdin'
                got = child.stdout.readline()
                assert got == expected + b'\n', (letter, got, expected)

            try:
                command(b'p', b'false false false')
                child.stdin.write(b'abc'); child.stdin.flush()
                command(b'p', b'false false false')
                child.stdin.write(b' def'); child.stdin.flush()
                command(b'p', b'true false false')
                command(b's', b'[abc]')
                command(b'p', b'false false false')
                child.stdin.write(b',tail\n'); child.stdin.flush()
                command(b'p', b'true true true')
                command(b'c', b'[def]')
                command(b'p', b'true true false')
                command(b'l', b'[tail]')
                command(b'p', b'false false false')
                child.stdin.write(b'last'); child.stdin.flush()
                command(b'p', b'false false false')
                child.stdin.close()
                command(b'p', b'true true true')
                command(b's', b'[last]')
                command(b'p', b'false false false')
            finally:
                child.kill()
                child.wait(timeout=5)
                selector.close()
        os.close(read_control)
        os.close(write_control)

    if platform.system() == 'Linux' and platform.machine() in ('x86_64', 'AMD64'):
        # Force the actual Windows x64 blocks through their native API test shims.
        for name in ('streams', 'files', 'seek'):
            destination = standard / 'os' / (name + '.oc')
            original = destination.read_text()
            destination.write_text(re.sub(r'asm (\w+/\w+)',
                                         lambda m: 'asm x86_64/linux' if m[1] == 'x86_64/windows'
                                         else 'asm riscv64/windows', original))
        shim = root / 'windows.o'
        result = run([clang, '-c', str(fixtures / 'windows_file_shim.c'), '-o', str(shim)], text=True)
        assert result.returncode == 0, result.stderr
        link_extra.append(str(shim))
        program(byte_program.replace('PATH', quoted),
                prepare=lambda: path.write_bytes(bytes((0, 127, 128, 255))))
        program('''func main() {
            file := os.Open(PATH, os.FileMode.Write).unwrap();
            file.Write("abc\\nlast").unwrap(); file.Close();
            file = os.Open(PATH, os.FileMode.ReadAppend).unwrap();
            if (file.Read().unwrap() != 97) panic "Windows read";
            file.Write("!").unwrap(); file.Close();
            file = os.Open(PATH, os.FileMode.Read).unwrap();
            scanner := io.NewScanner(file);
            if (!scanner.HasNextln().unwrap() || scanner.Scanln().unwrap() != "abc") panic "Windows readiness";
            readOutcome9 := scanner.Scann(9).unwrap(); bytes := readOutcome9.Bytes; end := readOutcome9.End;
            if (bytes -> string != "last!" || !end) panic "Windows short EOF";
            file.Close();
            input := io.NewScanner(os.Stdin().unwrap());
            if (!input.HasNextc(',').unwrap() || input.Scanc(',').unwrap() != "x") panic "Windows pipe";
            if (!input.HasNextln().unwrap() || input.Scanln().unwrap() != "y") panic "Windows line";
            if (input.HasNext().unwrap()) panic "Windows EOF";
            io.Println("ok");
        }'''.replace('PATH', quoted), b'x,y\n')

print('scanner, stream readiness, file modes and close tests passed')
