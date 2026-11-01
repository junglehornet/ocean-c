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
        ('func main() { defer main(); }',False,'backend'),
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
print('backend check validation passed')
