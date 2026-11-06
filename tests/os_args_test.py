"""The trusted os startup hook receives argv before Ocean main."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())


def run(args, env):
    return subprocess.run(args, env=env, text=True, capture_output=True, timeout=30)


with tempfile.TemporaryDirectory(prefix='ocean args ') as directory:
    root = Path(directory)
    standard, project = root / 'standard', root / 'project'
    (standard / 'os').mkdir(parents=True)
    project.mkdir()
    env = dict(os.environ, OCEAN_SRC=str(standard))
    (standard / 'os' / 'args.oc').write_text('''package os;
extern int puts(buffer<i8> text);
count: int;
func setArgs(stream buffer<i8>, argc int) {
    count = argc;
    extern.puts(stream);
    offset := 0;
    for (i := 0; i < argc - 1; i++) {
        for (stream[offset] != 0) { offset++; }
        offset++;
        extern.puts( (&stream[offset])->buffer<i8>);
    }
}
func Count() int { return count; }
''')
    source = project / 'main.oc'
    native = root / 'program'
    for imports, count in [('import "os";', 'os.Count()'),
                           ('import "os" as system;', 'system.Count()')]:
        source.write_text(f'package main; {imports} func main() int {{ return {count}; }}')
        built = run([compiler, 'build', str(source), '-o', str(native)], env)
        assert built.returncode == 0, built.stderr
        for arguments in [[], ['first', '', 'with spaces', 'héllo']]:
            result = run([str(native), *arguments], env)
            assert result.returncode == len(arguments) + 1, result
            assert result.stdout.splitlines() == [str(native), *arguments], result
        # The run command forwards the same argument stream.
        for location in [str(source), str(project), '.', 'project']:
            for arguments in [['arg1', 'arg2'],
                              ['--verbose', '-x', '', 'b c'],
                              ['--', '-o', '--emit-llvm', '--check', '--help'],
                              ['--', 'a', '', 'b c']]:
                result = subprocess.run(
                    [compiler, 'run', location, *arguments], env=env,
                    cwd=root if location == 'project' else project,
                    text=True, capture_output=True, timeout=30)
                forwarded = arguments[1:] if arguments[0] == '--' else arguments
                assert result.returncode == len(forwarded) + 1, result
                assert result.stdout.splitlines()[1:] == forwarded, result
    # Compiler flags remain compiler options even after program arguments.
    for option in [['-o', str(root / 'bad')], ['--emit-llvm'], ['--check']]:
        for prefix in [[], ['arg1', '--verbose']]:
            result = run([compiler, 'run', str(project), *prefix, *option], env)
            assert result.returncode == 1 and 'incompatible' in result.stderr, result
    result = run([compiler, 'run', str(project), 'arg1', '-o'], env)
    assert result.returncode == 1 and 'requires an output path' in result.stderr, result
    # Exercise the actual library implementation as well as the ABI probe.
    library = Path(__file__).resolve().parents[2] / 'ocean-src' / 'os' / 'args.oc'
    if library.exists():
        (standard / 'os' / 'args.oc').write_text(library.read_text())
        source.write_text('''package main; import "os";
func main() int {
    args := os.Args();
    if (len(args) != 4) return 1;
    if (args[1] != "") return 2;
    if (args[2] != "with spaces") return 3;
    if (args[3] != "héllo") return 4;
    return 0;
}''')
        result = run([compiler, 'run', str(source), '--', '', 'with spaces', 'héllo'], env)
        assert result.returncode == 0, result.stderr
    source.write_text('package main; import "os"; func main() { os.setArgs(null, 0); }')
    rejected = run([compiler, 'check', str(source)], env)
    assert rejected.returncode != 0 and 'not exported' in rejected.stderr, rejected.stderr
    source.write_text('package main; func main() {}')
    ir = root / 'plain.ll'
    emitted = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env)
    assert emitted.returncode == 0, emitted.stderr
    assert 'args.stream' not in ir.read_text()
print('os argument startup checks passed')
