"""Recoverable OS/format failures, successful EOF, and preserved panic contracts."""
from pathlib import Path
import json
import os
import platform
import re
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixtures = Path(__file__).with_name('fixtures')


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, timeout=40, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean recoverable ') as directory:
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
    env = dict(os.environ, OCEAN_SRC=str(standard))
    app = root / 'program'
    app.mkdir()
    source, ir, executable = app / 'main.oc', root / 'main.ll', root / 'app'
    path = root / 'data'
    quoted = json.dumps(str(path))
    link_extra = []

    def build(body):
        source.write_text('package main; import "io"; import "os";\n' + body.replace('PATH', quoted))
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env=env)
        assert result.returncode == 0, result.stdout + result.stderr

    def program(body, output=b'ok\n', error=None, data=b''):
        build(body)
        for opt in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', opt, str(ir), '-o', str(executable), '-lffi', *link_extra])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)], input=data)
            if error:
                assert result.returncode != 0 and error.encode() in result.stderr, result
            else:
                assert result.returncode == 0 and result.stdout == output, result

    program('''func main() {
        if (os.Open("missing\\0path", os.FileMode.Read) != Result<os.File>.error) panic "NUL open";
        if (os.Open(PATH + ".missing", os.FileMode.Read) != Result<os.File>.error) panic "missing open";
        file := os.Open(PATH, os.FileMode.Write).unwrap();
        if (file.Write("a\\0b").unwrap() != 3) panic "write count";
        if (file.Read() != Result<int>.error || file.Readn(2) != Result<os.ReadResult>.error ||
            file.Readall() != Result<array<i8>>.error || file.Readend() != Result<array<i8>>.error) panic "read mode errors";
        file.Close();
        if (file.Size() != Result<int>.error || file.Write("x") != Result<int>.error) panic "closed errors";
        file = os.Open(PATH, os.FileMode.Read).unwrap();
        if (file.Size().unwrap() != 3 || file.Read().unwrap() != 97) panic "read success";
        chunk := file.Readn(4).unwrap();
        if (len(chunk.Bytes) != 4 || !chunk.End || chunk.Bytes[0] != 0 || chunk.Bytes[1] != 98 || chunk.Bytes[2] != 0) panic "EOF payload";
        if (file.Readchunk(1).unwrap() -> string != "") panic "successful EOF";
        file.Setpos(0);
        if (file.Readall().unwrap().size != 3 || file.Read().unwrap() != 97) panic "whole-file result";
        file.Close(); file.Close();
        if (io.Sprintf("{}") != Result<string>.error || io.Sprintf("", 1) != Result<string>.error) panic "format errors";
        if (io.Printf("{}") != Result<int>.error || io.Printfln("", 1) != Result<int>.error) panic "print format errors";
        if (io.Sprintf("{}{}", 1, "x").unwrap() != "1x") panic "format value";
        io.Println("ok").unwrap();
    }''')

    program('''func main() {
        bad := os.FromDescriptor(2147483647, true, true);
        if (bad.Readchunk(1) != Result<i8[]>.error || bad.Read() != Result<int>.error ||
            bad.Write("x") != Result<int>.error || bad.PollRead() != Result<int>.error ||
            bad.Size() != Result<int>.error) panic "native errors";
        scanner := io.NewScanner(bad);
        if (scanner.HasNext() != Result<bool>.error || scanner.HasNextln() != Result<bool>.error ||
            scanner.HasNextc(',') != Result<bool>.error) panic "poll failure became false";
        if (scanner.Scan() != Result<string>.error || scanner.Scanln() != Result<string>.error ||
            scanner.Scanc(',') != Result<string>.error || scanner.Scann(2) != Result<io.ScanResult>.error) panic "scanner native errors";
        source := os.Stdin().unwrap(); source.Close();
        if (os.Stdin() != Result<os.File>.error) panic "closed standard stream";
        io.Println("ok").unwrap();
    }''')

    program('''func main() {
        scanner := io.NewScanner(os.Stdin().unwrap());
        if (scanner.HasNext().unwrap()) panic "empty EOF readiness";
        if (scanner.Scan().unwrap() != "") panic "EOF string";
        result := scanner.Scann(4).unwrap();
        if (len(result.Bytes) != 0 || !result.End) panic "EOF scan payload";
        io.Println("ok").unwrap();
    }''')
    program('''func main() {
        if (io.Print("a").unwrap() != 1) panic "Print count";
        if (io.Println("é").unwrap() != 3) panic "Println count";
        if (io.Printf("{}{}", 1, "x").unwrap() != 2) panic "Printf count";
        if (io.Printfln("{}", 2).unwrap() != 2) panic "Printfln count";
    }''', output='aé\n1x2\n'.encode())

    for body, message in (
        ('os.FromDescriptor(2147483647, true, true).Close();', 'file close failed'),
        ('os.Stdin().unwrap().Setpos(0);', 'file seek failed'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(-1);', 'position out of bounds'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Setpos(f.Size().unwrap()+1);', 'position out of bounds'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Readend().unwrap(); f.Read();', 'read past EOF'),
        ('f := os.Open(PATH, os.FileMode.Binary).unwrap(); io.NewScanner(f);', 'binary file'),
        ('f := os.Open(PATH, os.FileMode.Read).unwrap(); f.Readn(-1);', 'invalid read size'),
    ):
        program('func main() { ' + body + ' }', error=message)

    if os.name == 'posix':
        # Ignore SIGPIPE so kernel write errors can be observed as Results.
        guard = root / 'sigpipe.c'
        guard.write_text('#include <signal.h>\n__attribute__((constructor)) static void init(void) { signal(SIGPIPE,SIG_IGN); }\n')
        link_extra.append(str(guard))
        build('''func main() int {
            if (io.Print("x") != Result<int>.error) return 1;
            if (io.Println("x") != Result<int>.error) return 2;
            if (io.Printf("{}",1) != Result<int>.error) return 3;
            if (io.Printfln("{}",1) != Result<int>.error) return 4;
            return 0;
        }''')
        for opt in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', opt, str(ir), str(guard), '-o', str(executable), '-lffi'])
            assert result.returncode == 0, result.stderr
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                result = subprocess.run([str(executable)], stdout=write_fd, stderr=subprocess.PIPE, timeout=5)
                assert result.returncode == 0 and not result.stderr, result
            finally:
                os.close(write_fd)
        link_extra.clear()

    if platform.system() == 'Linux' and platform.machine() in ('x86_64', 'AMD64'):
        for name in ('streams', 'files', 'seek'):
            destination = standard / 'os' / (name + '.oc')
            destination.write_text(re.sub(r'asm (\w+/\w+)', lambda m: 'asm x86_64/linux'
                                         if m[1] == 'x86_64/windows' else 'asm riscv64/windows', destination.read_text()))
        shim = root / 'windows.o'
        result = run([clang, '-c', str(fixtures / 'windows_file_shim.c'), '-o', str(shim)])
        assert result.returncode == 0, result.stderr
        link_extra.append(str(shim))
        program('''func main() {
            file := os.Open(PATH, os.FileMode.Write).unwrap();
            file.Write("ab cd").unwrap(); file.Close();
            file = os.Open(PATH, os.FileMode.Read).unwrap();
            if (file.Read().unwrap() != 97) panic "initial cursor";
            asm x86_64/linux { mov edi, 1
                call FailReadAfter
            }
            if (file.Readall() != Result<array<i8>>.error) panic "read failure";
            asm x86_64/linux { mov edi, -1
                call FailReadAfter
            }
            if (file.Read().unwrap() != 98) panic "failed Readall lost cursor";
            file.Setpos(0);
            scanner := io.NewScanner(file);
            if (scanner.Scan().unwrap() != "ab") panic "buffer prime";
            asm x86_64/linux { mov edi, 2
                call FailSeekAfter
            }
            if (file.Readall() != Result<array<i8>>.error) panic "restore failure";
            asm x86_64/linux { mov edi, -1
                call FailSeekAfter
            }
            if (scanner.HasNext().unwrap()) panic "stale scanner buffer after failed restore";
            file.Close();
            io.Println("ok").unwrap();
        }''')

print('recoverable file, scanner, formatting and printing errors passed')
