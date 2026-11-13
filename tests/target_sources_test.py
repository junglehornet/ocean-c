"""Target variants must be excluded before parsing and import resolution."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
compiler=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ocean-target-sources-') as tmp:
    root=Path(tmp)
    (root/'main.oc').write_text('package main; func main() int { return Chosen(); }')
    native=root/'choice_native.oc';wasm=root/'choice_wasm.oc'
    native.write_text('package main; func Chosen() int { return 11; }')
    wasm.write_text('this is intentionally not valid Ocean syntax')
    env=dict(os.environ,OCEAN_BUILD_CACHE=str(root/'cache'))
    def run(*args,environment=env,expected=0):
        p=subprocess.run([str(compiler),*map(str,args)],env=environment,capture_output=True,text=True,timeout=30)
        assert p.returncode==expected,(args,p.returncode,p.stdout,p.stderr)
    output=root/('program.exe' if os.name=='nt' else 'program')
    run('build',root,'-o',output)
    p=subprocess.run([str(output)],timeout=10)
    assert p.returncode==11
    native.write_text('this is intentionally not valid Ocean syntax')
    wasm.write_text('package main; func Chosen() int { return 42; }')
    # An excluded native test must not pull the native-only testing runtime in.
    (root/'target_native_test.oc').write_text('this test is intentionally invalid')
    wasm_env=dict(env,OCOS='js',OCARCH='wasm')
    run('build',root,'--emit-llvm','-o',root/'wasm.ll',environment=wasm_env)
    run('check',root,environment=wasm_env)
    assert 'wasm32-unknown-unknown' in (root/'wasm.ll').read_text()
print('native/Wasm source and test-source filtering passed')
