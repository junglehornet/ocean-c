"""Persistent content-addressed IR/object caching, invalidation, and recovery."""
from pathlib import Path
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=30, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean build cache ') as temporary:
    root = Path(temporary)
    project, standard, cache, tools = (root / n for n in ('project', 'standard', 'cache', 'tools'))
    for p in (project, standard, tools): p.mkdir()
    log = root / 'clang.log'
    wrapper = tools / 'clang'
    wrapper.write_text('#!' + sys.executable + '\nimport os, sys\n'
                       'with open(os.environ["OCEAN_CACHE_TEST_LOG"], "a") as f: f.write(("compile" if "-c" in sys.argv else "link") + "\\n")\n'
                       'if os.environ.get("OCEAN_CACHE_TEST_FAIL") and "-c" in sys.argv: sys.exit(23)\n'
                       'os.execv(' + repr(clang) + ', [' + repr(clang) + ', *sys.argv[1:]])\n')
    wrapper.chmod(0o755)
    env = dict(os.environ, OCEAN_BUILD_CACHE=str(cache), OCEAN_SRC=str(standard),
               OCEAN_CACHE_TEST_LOG=str(log), PATH=str(tools) + os.pathsep + os.environ.get('PATH', ''))
    source, output, ir = project / 'main.oc', root / 'program', root / 'main.ll'
    source.write_text('package main; func main() int { return 7; }')
    checks = 0

    def invoke(*args, status=0, environment=None, executable=compiler, cwd=project):
        global checks
        result = run([executable, *map(str, args)], env=environment or env, cwd=cwd)
        assert result.returncode == status, (args, result)
        checks += 1
        return result

    def counts():
        rows = log.read_text().splitlines() if log.exists() else []
        result = rows.count('compile'), rows.count('link')
        return result

    def actions():
        return {str(p.relative_to(cache)): (p.stat().st_ino, p.read_bytes())
                for p in (cache / 'v1/actions').glob('*/*') if p.is_file()}

    def integrity():
        for p in (cache / 'v1/data').glob('*/*'):
            assert hashlib.sha256(p.read_bytes()).hexdigest() == p.name, p
        for p in (cache / 'v1/actions').glob('*/*'):
            data = p.read_bytes()
            assert len(data) == 65 and data[-1:] == b'\n' and len(bytes.fromhex(data[:-1].decode())) == 32, p
        assert not list(cache.rglob('*.cache-*'))

    invoke('build', '--emit-llvm', source, '-o', ir)
    assert counts() == (0, 0)
    first = actions(); assert len(first) == 1
    original_ir = ir.read_bytes()
    invoke('build', '--emit-llvm', source, '-o', root / 'second.ll')
    assert (root / 'second.ll').read_bytes() == original_ir and actions() == first
    invoke('build', source, '-o', output)
    assert counts() == (2, 1) and run([str(output)]).returncode == 7
    warm = actions(); assert len(warm) == 3
    invoke('build', source, '-o', root / 'other output')
    assert counts() == (2, 2) and actions() == warm
    invoke('run', source, '--', 'argument', status=7)
    assert counts() == (2, 3) and actions() == warm
    invoke('check', source)
    assert counts() == (2, 4) and actions() == warm
    integrity()

    # Content changes invalidate even with the original modification timestamp.
    old_stat = source.stat()
    source.write_text('package main; func main() int { return 8; }')
    os.utime(source, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
    invoke('build', source, '-o', output)
    assert counts() == (4, 5) and run([str(output)]).returncode == 8
    source.write_text('package main; func main() int { return 7; }')
    invoke('build', source, '-o', output)
    assert counts() == (4, 6)

    # Corrupt entries are repaired, not used or reported as build errors.
    ir_manifest = next(p for p in (cache / 'v1/actions').glob('*/*') if p.read_bytes() == next(iter(first.values()))[1])
    ir_data = cache / 'v1/data' / ir_manifest.read_text()[:2] / ir_manifest.read_text().strip()
    ir_data.write_bytes(b'corrupt IR')
    invoke('build', source, '-o', output)
    assert counts() == (4, 7) and ir_data.read_bytes() == original_ir
    object_manifest = next(p for p in (cache / 'v1/actions').glob('*/*')
                           if str(p.relative_to(cache)) in warm and str(p.relative_to(cache)) not in first)
    object_sum = object_manifest.read_text().strip()
    object_data = cache / 'v1/data' / object_sum[:2] / object_sum
    object_data.write_bytes(b'corrupt object')
    invoke('build', source, '-o', output)
    assert counts() == (5, 8) and run([str(output)]).returncode == 7
    ir_manifest.write_bytes(b'bad manifest')
    invoke('build', '--emit-llvm', source, '-o', ir)
    assert ir.read_bytes() == original_ir
    integrity()

    # Dependency edits, additions, removals, and standard-library changes.
    (project / 'ocean.mod').write_text('module example.com/project\nocean 0.1\n')
    dependency = project / 'dependency'; dependency.mkdir()
    dep = dependency / 'value.oc'; dep.write_text('package dependency; func Value() int { return 2; }')
    package = standard / 'sample'; package.mkdir()
    std = package / 'value.oc'; std.write_text('package sample; func Value() int { return 3; }')
    source.write_text('package main; import "example.com/project/dependency"; import "sample"; func main() int { return dependency.Value() + sample.Value(); }')
    invoke('build', source, '-o', output)
    c, l = counts(); assert run([str(output)]).returncode == 5
    dep.write_text('package dependency; func Value() int { return 4; }')
    invoke('build', source, '-o', output)
    assert counts() == (c + 1, l + 1) and run([str(output)]).returncode == 7
    std.write_text('package sample; func Value() int { return 5; }')
    invoke('build', source, '-o', output)
    assert counts() == (c + 2, l + 2) and run([str(output)]).returncode == 9
    extra = dependency / 'extra.oc'; extra.write_text('package dependency; func Extra() int { return 6; }')
    invoke('build', source, '-o', output)
    assert counts() == (c + 5, l + 3)
    extra.unlink()
    invoke('build', source, '-o', output)
    assert counts() == (c + 5, l + 4)

    # Tool/environment identities invalidate objects, without invalidating IR.
    before = actions(); c, l = counts()
    wrapper.write_text(wrapper.read_text() + '# new tool identity\n')
    invoke('build', source, '-o', output)
    assert counts() == (c + 4, l + 1)
    changed_env = dict(env, OCEAN_CACHE_TEST_SETTING='changed')
    invoke('build', source, '-o', output, environment=changed_env)
    assert counts() == (c + 8, l + 2)
    copied_compiler = root / 'ocean-copy'; shutil.copy2(compiler, copied_compiler)
    before = actions()
    invoke('build', '--emit-llvm', source, '-o', ir, executable=str(copied_compiler))
    assert actions() == before
    with copied_compiler.open('ab') as file: file.write(b'\0')
    invoke('build', '--emit-llvm', source, '-o', ir, executable=str(copied_compiler))
    assert len(actions()) == len(before) + 1
    before = len(actions())
    invoke('build', '--emit-llvm', source, '-o', ir, cwd=root)
    assert len(actions()) == before + 1

    # Library .o outputs share the object cache too.
    library = root / 'library'; library.mkdir()
    (library / 'lib.oc').write_text('package library; func Value() int { return 9; }')
    c, l = counts()
    invoke('build', library, '-o', root / 'library.o')
    invoke('build', library, '-o', root / 'library-copy.o')
    assert counts() == (c + 2, l + 2) and (root / 'library.o').read_bytes() == (root / 'library-copy.o').read_bytes()

    # Disabled/unavailable caches leave compilation and errors unchanged.
    disabled = dict(env, OCEAN_BUILD_CACHE='off'); before = actions()
    invoke('build', source, '-o', output, environment=disabled)
    assert actions() == before and run([str(output)]).returncode == 9
    blocked = root / 'not-a-directory'; blocked.write_text('keep')
    invoke('build', source, '-o', output, environment=dict(env, OCEAN_BUILD_CACHE=str(blocked)))
    assert blocked.read_text() == 'keep'
    before = actions(); saved_output = output.read_bytes()
    source.write_text('package main; func main() int { return missing; }')
    invoke('build', source, '-o', output, status=1)
    assert actions() == before and output.read_bytes() == saved_output
    source.write_text('package main; extern int missing(); func main() int { return extern.missing(); }')
    invoke('build', source, '-o', output, status=1)
    invoke('build', source, '-o', output, status=1)
    assert output.read_bytes() == saved_output

    source.write_text('package main; func main() int { return 13; }')
    c, l = counts()
    failing = dict(env, OCEAN_CACHE_TEST_FAIL='yes')
    invoke('build', source, '-o', output, status=1, environment=failing)
    first_failure = actions()
    invoke('build', source, '-o', output, status=1, environment=failing)
    assert counts() == (c + 2, l) and actions() == first_failure
    assert output.read_bytes() == saved_output

    # Concurrent writers publish complete entries and independent outputs.
    source.write_text('package main; func main() int { return 11; }')
    def concurrent_build(i):
        target = root / f'parallel-{i}'
        result = run([compiler, 'build', str(source), '-o', str(target)], env=env, cwd=project)
        assert result.returncode == 0, result
        assert run([str(target)]).returncode == 11
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        list(pool.map(concurrent_build, range(6)))
    integrity()

    # Cleaning removes only the versioned cache, without following symlinks.
    unrelated = cache / 'keep'; unrelated.write_text('keep')
    outside = root / 'outside'; outside.mkdir(); (outside / 'keep').write_text('safe')
    (cache / 'v1' / 'outside-link').symlink_to(outside, target_is_directory=True)
    invoke('clean', '--cache')
    assert not (cache / 'v1').exists() and unrelated.read_text() == 'keep' and (outside / 'keep').read_text() == 'safe'
    invoke('clean', '-cache')
    invoke('build', source, '-o', output)
    assert run([str(output)]).returncode == 11
    integrity()

print(f'{checks} build cache checks passed')
