"""Source-keyed package objects, direct LLVM modules, invalidation and recovery."""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=60, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean source cache ') as temporary:
    root = Path(temporary)
    project, standard, cache, tools, captures = (root / n for n in ('project', 'standard', 'cache', 'tools', 'captures'))
    for p in (project, standard, tools, captures): p.mkdir()
    log = root / 'clang.jsonl'
    wrapper = tools / 'clang'
    wrapper.write_text('#!' + sys.executable + '\n' + '''import json, os, pathlib, shutil, sys, tempfile
args = sys.argv[1:]
record = {"compile": "-c" in args, "args": args}
if record["compile"]:
    source = next(a for a in args if a.endswith(".ll"))
    fd, saved = tempfile.mkstemp(suffix=".ll", dir=os.environ["OCEAN_CACHE_CAPTURES"])
    os.close(fd)
    shutil.copyfile(source, saved)
    record.update(module=pathlib.Path(source).name, saved=saved)
with open(os.environ["OCEAN_CACHE_TEST_LOG"], "a") as f:
    f.write(json.dumps(record) + "\\n")
if record["compile"] and os.environ.get("OCEAN_CACHE_TEST_FAIL"):
    sys.exit(23)
''' + 'os.execv(' + repr(clang) + ', [' + repr(clang) + ', *args])\n')
    wrapper.chmod(0o755)
    env = dict(os.environ, OCEAN_BUILD_CACHE=str(cache), OCEAN_SRC=str(standard),
               OCEAN_CACHE_TEST_LOG=str(log), OCEAN_CACHE_CAPTURES=str(captures),
               PATH=str(tools) + os.pathsep + os.environ.get('PATH', ''))
    source, output, ir = project / 'main.oc', root / 'program', root / 'main.ll'
    original = 'package main; func main() int { return 7; }'
    source.write_text(original)
    checks = 0

    def invoke(*args, status=0, environment=None, executable=compiler, cwd=project):
        global checks
        result = run([executable, *args], env=environment or env, cwd=cwd)
        assert result.returncode == status, (args, result)
        checks += 1
        return result

    def records():
        return [json.loads(row) for row in log.read_text().splitlines()] if log.exists() else []

    def compiles():
        return [r for r in records() if r['compile']]

    def built(expected=0):
        result = run([output])
        assert result.returncode == expected, result
        return result

    def build_delta(expected, status=0, environment=None):
        before = len(compiles())
        invoke('build', source, '-o', output, status=status, environment=environment)
        added = compiles()[before:]
        assert len(added) == expected, (expected, added)
        return added

    def actions():
        return {str(p.relative_to(cache)): p.read_bytes() for p in (cache / 'v1/actions').glob('*/*')}

    def integrity():
        for p in (cache / 'v1/data').glob('*/*'):
            assert hashlib.sha256(p.read_bytes()).hexdigest() == p.name, p
        for value in actions().values():
            assert len(value) == 65 and value[-1:] == b'\n' and len(bytes.fromhex(value[:-1].decode())) == 32
        assert not list(cache.rglob('*.cache-*'))

    # Native builds do not emit/cache merged LLVM at all. First build has one
    # object per package plus program support; warm builds perform only linking.
    cold = build_delta(2); built(7)
    assert {r['module'] for r in cold} == {'support.ll', 'package-1.ll'}
    native_actions = actions(); assert len(native_actions) == 3
    for manifest in native_actions.values():
        digest = manifest.decode().strip()
        data = (cache / 'v1/data' / digest[:2] / digest).read_bytes()
        assert data.startswith(b'\x7fELF') or data in (b'0', b'1')
    build_delta(0); built(7)
    assert actions() == native_actions
    invoke('run', source, status=7)
    assert len(compiles()) == 2
    before = len(compiles()); invoke('check', source)
    assert len(compiles()) == before + 2 and actions() == native_actions

    # Debugging LLVM remains an explicit, independently source-keyed action.
    invoke('build', '--emit-llvm', source, '-o', ir)
    original_ir = ir.read_bytes(); all_actions = actions()
    assert len(all_actions) == len(native_actions) + 1
    invoke('build', '--emit-llvm', source, '-o', root / 'second.ll')
    assert (root / 'second.ll').read_bytes() == original_ir and actions() == all_actions

    # Source changes invalidate even if generated package LLVM is byte-identical
    # and even if mtimes are restored. Native cache lookup never hashes IR.
    previous = next(Path(r['saved']).read_bytes() for r in cold if r['module'] != 'support.ll')
    old_stat = source.stat()
    source.write_text(original + '\n// no code generation changes\n')
    os.utime(source, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
    changed = build_delta(2); built(7)
    current = next(Path(r['saved']).read_bytes() for r in changed if r['module'] != 'support.ll')
    assert previous == current
    source.write_text(original); build_delta(0)
    source.write_text(original.replace('return 7', 'return 8')); build_delta(2); built(8)
    source.write_text(original); build_delta(0); built(7)

    # Corrupt native artifacts are repaired; a corrupt debug IR entry is not
    # consulted by a native cache hit and is repaired only on explicit emission.
    object_row = next(p for p in (cache / 'v1/actions').glob('*/*')
                      if str(p.relative_to(cache)) in native_actions and
                      (cache / 'v1/data' / p.read_text()[:2] / p.read_text().strip()).read_bytes().startswith(b'\x7fELF'))
    object_data = cache / 'v1/data' / object_row.read_text()[:2] / object_row.read_text().strip()
    object_data.write_bytes(b'corrupt object')
    build_delta(1); built(7)
    debug_row = next(p for p in (cache / 'v1/actions').glob('*/*')
                     if str(p.relative_to(cache)) in all_actions and str(p.relative_to(cache)) not in native_actions)
    debug_data = cache / 'v1/data' / debug_row.read_text()[:2] / debug_row.read_text().strip()
    debug_data.write_bytes(b'corrupt IR')
    build_delta(0)
    assert debug_data.read_bytes() == b'corrupt IR'
    invoke('build', '--emit-llvm', source, '-o', ir)
    assert ir.read_bytes() == original_ir and debug_data.read_bytes() == original_ir
    integrity()

    (project / 'ocean.mod').write_text('module example.com/project\nocean 0.1\n')
    dependency = project / 'dependency'; dependency.mkdir()
    dep = dependency / 'value.oc'
    dep.write_text('package dependency; func Value() int { return 2; } func Other() int { return 4; }')
    sample = standard / 'sample'; sample.mkdir()
    std = sample / 'value.oc'; std.write_text('package sample; func Value() int { return 3; }')
    application = 'package main; import "example.com/project/dependency"; import "sample"; func main() int { return dependency.Value() + sample.Value(); }'
    source.write_text(application)
    added = build_delta(4); built(5)
    # Definitions are routed directly: ordinary package functions do not appear
    # as definitions in support, and runtime helpers are declared in packages.
    support_ir = next(Path(r['saved']).read_text() for r in added if r['module'] == 'support.ll')
    assert not any(line.startswith('define ') and '__package_' in line for line in support_ir.splitlines())
    assert all(not any(line.startswith('define ') and '@__ocean.enter(' in line
                       for line in Path(r['saved']).read_text().splitlines())
               for r in added if r['module'] != 'support.ll')
    # Reuse the complete dependency object when the caller starts using Other.
    source.write_text(application.replace('dependency.Value()', 'dependency.Other()'))
    build_delta(2); built(7)
    source.write_text(application); build_delta(0); built(5)
    dep.write_text(dep.read_text().replace('return 2', 'return 6'))
    build_delta(3); built(9)  # dependency, importer, support; sample stays cached
    std.write_text(std.read_text().replace('return 3', 'return 5'))
    build_delta(3); built(11)
    extra = dependency / 'extra.oc'; extra.write_text('package dependency; func Extra() int { return 10; }')
    build_delta(3); built(11)
    extra.unlink(); build_delta(0); built(11)

    # Type IDs and generated names must survive unrelated source/type additions
    # and package discovery-order changes while dependency objects stay cached.
    dep.write_text('''package dependency;
        struct Box<T> { value T; }
        func Make() Box<int> { return Box<int>{7}; }
        func Boxed() any { boxed: any = Make(); return boxed; }
        func Check(value any) int { return value.(Box<int>).value; }
        func identity<T>(value T) T { return value; }
        func (box Box<T>) Get() T { return box.value; }
        func Number() int { x := identity<Box<int>>(Make()); return x.Get(); }
        func MakeMap() map[string]int { return map[string]int{"answer": 7}; }
        func Callback() func() int { return func() int { return 7; }; }
        func Cleanup() {}
        func Deferred() int { defer Cleanup(); return 7; }
    ''')
    application = '''package main; import "example.com/project/dependency";
        func main() int {
            a := dependency.Make(); b := dependency.Boxed();
            m := dependency.MakeMap(); if (m["answer"] != 7) return 2;
            f := dependency.Callback();
            if (a.value != 7 || dependency.Check(b) != 7 || f() != 7 ||
                dependency.Number() != 7 || dependency.Deferred() != 7) return 1;
            return 0;
        }'''
    source.write_text(application); build_delta(3); built()
    source.write_text(application.replace('func main()', 'struct Unrelated { words string[]; } func main()'))
    build_delta(2); built()
    other = project / 'another'; other.mkdir(); (other / 'other.oc').write_text('package another; func Value() int { return 0; }')
    source.write_text(application.replace('import "example.com/project/dependency";',
                       'import "example.com/project/another"; import "example.com/project/dependency";'))
    build_delta(3); built()

    # Object identity accounts for toolchain and environment, without touching
    # the explicitly emitted LLVM cache. Compiler bytes also invalidate keys.
    count = len(compiles()); wrapper.write_text(wrapper.read_text() + '# tool identity changed\n')
    build_delta(4); assert len(compiles()) == count + 4
    build_delta(4, environment=dict(env, OCEAN_CACHE_TEST_SETTING='changed'))
    before = actions()
    copied = root / 'compiler-copy'; shutil.copy2(compiler, copied)
    invoke('build', '--emit-llvm', source, '-o', ir)
    before = actions(); invoke('build', '--emit-llvm', source, '-o', ir, executable=str(copied))
    assert actions() == before
    with copied.open('ab') as file: file.write(b'\0')
    invoke('build', '--emit-llvm', source, '-o', ir, executable=str(copied))
    assert len(actions()) == len(before) + 1

    library = root / 'library'; library.mkdir()
    (library / 'lib.oc').write_text('package library; func Value() int { return 9; }')
    count = len(compiles())
    invoke('build', library, '-o', root / 'library.o')
    invoke('build', library, '-o', root / 'library-copy.o')
    assert len(compiles()) == count + 2 and (root / 'library.o').read_bytes() == (root / 'library-copy.o').read_bytes()

    # Cache failure is a miss, not a failure of the program being compiled.
    before = actions()
    build_delta(4, environment=dict(env, OCEAN_BUILD_CACHE='off'))
    assert actions() == before
    blocked = root / 'not-a-directory'; blocked.write_text('keep')
    build_delta(4, environment=dict(env, OCEAN_BUILD_CACHE=str(blocked)))
    assert blocked.read_text() == 'keep'
    saved_output = output.read_bytes()
    source.write_text('package main; func main() int { return missing; }')
    invoke('build', source, '-o', output, status=1)
    assert output.read_bytes() == saved_output
    source.write_text('package main; extern int missing(); func main() int { return extern.missing(); }')
    invoke('build', source, '-o', output, status=1)
    invoke('build', source, '-o', output, status=1)
    assert output.read_bytes() == saved_output
    source.write_text('package main; func main() int { return 13; }')
    failing = dict(env, OCEAN_CACHE_TEST_FAIL='yes')
    build_delta(1, status=1, environment=failing)
    build_delta(1, status=1, environment=failing)
    assert output.read_bytes() == saved_output

    source.write_text('package main; func main() int { return 11; }')
    def concurrent_build(i):
        target = root / f'parallel-{i}'
        result = run([compiler, 'build', source, '-o', target], env=env, cwd=project)
        assert result.returncode == 0, result
        assert run([target]).returncode == 11
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(concurrent_build, range(6)))
    integrity()
    unrelated = cache / 'keep'; unrelated.write_text('keep')
    outside = root / 'outside'; outside.mkdir(); (outside / 'keep').write_text('safe')
    (cache / 'v1' / 'outside-link').symlink_to(outside, target_is_directory=True)
    invoke('clean', '--cache')
    assert not (cache / 'v1').exists() and unrelated.read_text() == 'keep' and (outside / 'keep').read_text() == 'safe'
    invoke('clean', '-cache'); build_delta(2); built(11); integrity()

print(f'{checks} source package cache checks passed')
