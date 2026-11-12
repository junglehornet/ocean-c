#!/usr/bin/env python3
"""Serial package phases, parked Parallel markers, and command-wide worker slots."""
import json,os,subprocess,sys,tempfile
from pathlib import Path
compiler=Path(sys.argv[1]).resolve()
stdlib=Path(os.environ.get('OCEAN_SRC',compiler.parent.parent/'ocean-src')).resolve()
env={**os.environ,'OCEAN_SRC':str(stdlib),'OCEAN_BUILD_CACHE':'off','NO_COLOR':''}
with tempfile.TemporaryDirectory(prefix='ocean-phases-') as tmp:
 root=Path(tmp); (root/'ocean.mod').write_text('module example.test/phases\n\nocean 0.1\n')
 def write(name,n=20):
  folder=root/name;folder.mkdir(exist_ok=True)
  text='''package phase; import "testing"; import "time";
func TestSerialFirst(t *testing.T) { t.Log("serial start"); time.Sleep(500 * time.Millisecond); }
'''
  for i in range(n):
   text+=f'func TestParallel{i:02d}(t *testing.T) {{ t.Parallel(); time.Sleep(80 * time.Millisecond); }}\n'
  text+='func TestSerialLast(t *testing.T) { t.Log("last serial"); }\n'
  (folder/'phase_test.oc').write_text(text)
  return folder
 a=write('a'); b=write('b')
 def run(paths,options=(),bad=False):
  r=subprocess.run([str(compiler),'test',*map(str,paths),'--json','--timeout','8s',*options],env=env,capture_output=True,text=True,timeout=30)
  assert (r.returncode!=0)==bad,(r.returncode,r.stdout,r.stderr)
  result=[json.loads(line) for line in r.stdout.splitlines()]
  assert all(e['Action'] not in ('acquire','release') for e in result)
  return result
 def verify(events,limit,expected):
  active=set();peak=0;serial=set();last=set();paused={};resumed={}
  for e in events:
   p=e['Package'];name=e.get('Test');action=e['Action']
   if not name:continue
   key=(p,name)
   if action=='run':
    assert p not in serial,('overlapping serial tests',e)
    serial.add(p);active.add(key)
   elif action=='pause':
    assert p in serial
    serial.remove(p);active.remove(key);paused[key]=True
   elif action=='cont':
    assert p in last,('resumed before serial phase ended',e)
    assert key in paused and key not in resumed
    resumed[key]=True;active.add(key)
   elif action in ('pass','fail','skip'):
    active.discard(key)
    if name=='TestSerialLast':last.add(p)
    if key not in resumed:serial.discard(p)
   peak=max(peak,len(active));assert len(active)<=limit,(limit,e,active)
  assert not active and len(resumed)==expected,(peak,len(resumed),expected)
  return peak
 for options,limit in [((),16),(('-t','3'),3),(('--single-threaded',),1)]:
  events=run([a],options);assert verify(events,limit,20)==limit
 events=run([a,b],('-t','3'));assert verify(events,3,40)==3
 # Both packages' first serial tests must overlap, proving they do not run in selection order.
 starts=[(i,e) for i,e in enumerate(events) if e.get('Test')=='TestSerialFirst' and e['Action']=='run']
 first_end=min(i for i,e in enumerate(events) if e.get('Test')=='TestSerialFirst' and e['Action']=='pass')
 assert len(starts)==2 and max(i for i,e in starts)<first_end
 # Run waits only until a child pauses; the child resumes after the parent's body exits.
 (a/'phase_test.oc').write_text('''package phase; import "testing"; import "time";
func TestParent(t *testing.T) {
    done := false;
    t.Run("child", func(t *testing.T) { t.Parallel(); done = true; t.Error("child failure"); });
    if (done) t.Fatal("parallel child resumed before parent body returned");
    t.Cleanup(func() { if (!done) t.Error("cleanup before parallel child"); t.Log("parent cleanup"); });
    t.Log("parent body done");
}
func TestDouble(t *testing.T) { t.Parallel(); t.Parallel(); }
func TestFatal(t *testing.T) { t.Parallel(); defer func(){t.Log("fatal defer");}(); t.Fatal("parallel fatal"); }
func TestLater(t *testing.T) { t.Log("later serial ran"); }
''')
 events=run([a],('--single-threaded',),True)
 messages=''.join(e.get('Output','') for e in events)
 assert 'parallel child resumed before parent body returned' not in messages
 assert 'cleanup before parallel child' not in messages
 assert all(x in messages for x in ('parent body done','parent cleanup','child failure','fatal defer','later serial ran','Parallel called twice'))
 parent_end=next(i for i,e in enumerate(events) if e.get('Test')=='TestParent' and e['Action']=='fail')
 child_end=next(i for i,e in enumerate(events) if e.get('Test')=='TestParent/child' and e['Action']=='fail')
 assert child_end<parent_end
 # A crashing package must return its held slots to packages still waiting.
 (a/'phase_test.oc').write_text('package phase; import "testing"; import "os"; func TestExit(t *testing.T) { os.Exit(7); }')
 events=run([a,b],('-t','2'),True)
 assert any(e['Package'].endswith('/b') and e['Action']=='pass' and 'Test' not in e for e in events)
 (a/'phase_test.oc').write_text('package phase; import "testing"; func TestHang(t *testing.T) { for (;;) {} }')
 events=run([a,b],('-t','2','--timeout','1s'),True)
 assert any('timed out' in e.get('Output','') for e in events)
 for options in [('-t',),('-t','0'),('-t','-1'),('-t','2x')]:
  r=subprocess.run([str(compiler),'test',*options,str(a)],env=env,capture_output=True,text=True)
  assert r.returncode!=0 and 'thread count' in r.stderr
print('Test scheduling phase checks passed')
