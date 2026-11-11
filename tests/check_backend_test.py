"""Check validates lowering, LLVM, and linkage without publishing or executing."""
from pathlib import Path
import os, subprocess, sys, tempfile
compiler=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='ocean check backend ') as d:
    root=Path(d);source=root/'main.oc';cache=root/'cache'
    env=dict(os.environ,OCEAN_BUILD_CACHE=str(cache))
    cases=[
        ('func main() int { return 7; }',True,''),
        ('func main() { x := 18446744073709551615; }',True,''),
        ('extern int ocean_missing_symbol(); func main() int { return extern.ocean_missing_symbol(); }',False,'link'),
        ('func cleanup() {} func main() { defer cleanup(); }',True,''),
        ('func main() int { return -9223372036854775808; }',True,''),
        ('func main() int { Ωmega := 1e2; return (Ωmega->int); }',True,''),
        ('func main() { panic "must never execute"; }',True,''),
    ]
    for body,ok,message in cases:
        source.write_text('package main; '+body)
        before=set(root.iterdir())
        for args in [('check',),('build','--check')]:
            r=subprocess.run([compiler,*args,str(source)],cwd=root,env=env,capture_output=True,text=True,timeout=20)
            assert (r.returncode==0)==ok,(body,r)
            assert not message or message in r.stderr,(body,r)
            assert set(root.iterdir())==before and not cache.exists(),list(root.iterdir())

# Test files are checked (including external packages), but never executed or
# included in ordinary builds. Invalid test entry signatures use test discovery.
with tempfile.TemporaryDirectory(prefix='ocean check tests ') as d:
    root = Path(d)
    (root/'ocean.mod').write_text('module example.test/checktests\n\nocean 0.1\n')
    (root/'main.oc').write_text('package main; func main() {} func Answer() int { return 42; }')
    test = root/'main_test.oc'
    env = dict(os.environ, OCEAN_SRC=os.environ.get('OCEAN_SRC', str(Path(__file__).resolve().parents[2]/'ocean-src')))
    cases = [
        ('package main; func helper() int { return false; }', False, 'type mismatch'),
        ('package main; func helper() { x := ; }', False, ''),
        ('package main; func TestBad() {}', False, 'must have signature'),
        ('package main; import "testing"; extern int ocean_missing_test_symbol(); func TestLink(t *testing.T) { extern.ocean_missing_test_symbol(); }', False, 'link'),
        ('package main_test; func helper() int { return false; }', False, 'type mismatch'),
        ('package main; import "testing"; func TestOK(t *testing.T) { panic "never execute"; }', True, ''),
        ('package main_test; import "testing"; import "example.test/checktests" as app; func TestOK(t *testing.T) { if (app.Answer() != 42) { t.Fail(); } }', True, ''),
    ]
    for body, ok, message in cases:
        test.write_text(body)
        for args in [('check',), ('build', '--check')]:
            result = subprocess.run([compiler, *args, str(root)], env=env, capture_output=True, text=True, timeout=30)
            assert (result.returncode == 0) == ok, (body, result)
            assert message in result.stderr, result
        result = subprocess.run([compiler, 'build', str(root), '-o', str(root/'app')], env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result

    # Imported dependencies' tests remain outside the selected package's check.
    dep = root/'dep'
    dep.mkdir()
    (dep/'dep.oc').write_text('package dep; func Value() int { return 1; }')
    (dep/'broken_test.oc').write_text('invalid Ocean source')
    (root/'main.oc').write_text('package main; import "example.test/checktests/dep"; func main() { dep.Value(); }')
    test.unlink()
    result = subprocess.run([compiler, 'check', str(root)], env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result
print('backend and test-file check validation passed')
