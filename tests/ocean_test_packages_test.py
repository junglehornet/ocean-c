"""Multiple selections, recursive discovery, report preservation and package UI identity."""
import os, sys, json, tempfile, subprocess, pty, termios, fcntl, struct, select, time
from pathlib import Path
compiler=Path(sys.argv[1]).resolve()
stdlib=Path(os.environ.get('OCEAN_SRC', compiler.parent.parent/'ocean-src')).resolve()
with tempfile.TemporaryDirectory(prefix='ocean-packages-') as temp:
 root=Path(temp); (root/'ocean.mod').write_text('module example.test/project\n\nocean 0.1\n')
 for name in ('a','a/sub','b'):
  folder=root/name; folder.mkdir(parents=True,exist_ok=True)
  (folder/'suite_test.oc').write_text('package suite; import "testing"; import "time"; import "io"; func TestSame(t *testing.T) { io.Println("raw package output"); t.Run("child", func(t *testing.T) {}); time.Sleep(100000000); }')
 env={**os.environ,'OCEAN_SRC':str(stdlib),'NO_COLOR':'','TERM':'xterm','OCEAN_BUILD_CACHE':str(root/'cache')}
 def run(*args, bad=False):
  r=subprocess.run([str(compiler),'test',*map(str,args)],cwd=root,env=env,capture_output=True,text=True,timeout=40)
  assert (r.returncode!=0)==bad,(r.stdout,r.stderr)
  return r
 def packages(r): return sorted(e['Package'] for e in map(json.loads,r.stdout.splitlines()) if e['Action']=='start')
 assert packages(run('a','b','--json'))==['example.test/project/a','example.test/project/b']
 assert packages(run('example.test/project/a/...','--json'))==['example.test/project/a','example.test/project/a/sub']
 assert packages(run('./...','--json'))==['example.test/project/a','example.test/project/a/sub','example.test/project/b']
 assert packages(run('a/...','a','a/sub','--json'))==['example.test/project/a','example.test/project/a/sub']
 report=root/'report.txt'; events=root/'events.jsonl'
 r=run('a','b','--json','--outfile',report,'--jsonfile',events)
 assert r.stdout==events.read_text()
 assert 'example.test/project/a' in report.read_text() and 'example.test/project/b' in report.read_text()
 assert report.read_text().count('raw package output') == 2
 assert report.read_text().count('─── Summary') == 1
 assert '(2 packages)' in report.read_text()
 totals=report.read_text().split('(2 packages)',1)[1].splitlines()[0].split()
 assert totals[:4]==['4','0','0','4'],totals
 text=run('a','b').stdout
 assert text.count('─── Summary')==1 and '(2 packages)' in text
 run('a','b','--outfile',root/'b/suite_test.oc',bad=True)
 assert (root/'b/suite_test.oc').read_text().startswith('package suite;')
 (root/'a/suite_test.oc').write_text('package suite; import "testing"; func TestSame(t *testing.T) { t.Fatal("failure"); }')
 failed_report=run('a','b',bad=True).stdout
 assert failed_report.count('─── Summary')==1 and '(2 packages)' in failed_report and 'FAIL' in failed_report
 assert packages(run('a','b','--json',bad=True))==['example.test/project/a','example.test/project/b']
 (root/'empty').mkdir(); run('empty/...',bad=True)
 master,slave=pty.openpty(); old=termios.tcgetattr(slave)
 fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',32,130,0,0))
 p=subprocess.Popen([str(compiler),'test','a','b'],cwd=root,env=env,stdin=slave,stdout=slave,stderr=slave)
 output=b''; deadline=time.monotonic()+40
 try:
  while p.poll() is None:
   assert time.monotonic()<deadline
   if select.select([master],[],[],.1)[0]: output+=os.read(master,65536)
  while select.select([master],[],[],0)[0]: output+=os.read(master,65536)
  assert p.wait()==1 and termios.tcgetattr(slave)==old
  frames=output.split(b'\x1b[J')
  assert any(b'example.test/project/a' in f and b'example.test/project/b' in f and b'=== RUN' in f and b'TestSame/child' in f for f in frames),output[-5000:]
 finally:
  if p.poll() is None: p.kill(); p.wait()
  os.close(master); os.close(slave)
 # Cancelling the shared TUI must terminate every package supervisor and restore raw mode.
 for folder in (root/'a',root/'b'):
  (folder/'suite_test.oc').write_text('package suite; import "testing"; func TestHang(t *testing.T) { for (;;) {} }')
 master,slave=pty.openpty(); old=termios.tcgetattr(slave)
 fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',32,130,0,0))
 p=subprocess.Popen([str(compiler),'test','a','b','--jsonfile',str(events)],cwd=root,env=env,stdin=slave,stdout=slave,stderr=slave)
 output=b''; deadline=time.monotonic()+30; sent=False
 try:
  while p.poll() is None:
   assert time.monotonic()<deadline
   if select.select([master],[],[],.1)[0]: output+=os.read(master,65536)
   if not sent and b'example.test/project/a' in output and b'example.test/project/b' in output and b'TestHang' in output:
    os.write(master,b'\x03'); sent=True
  assert sent and p.wait()==130,(p.returncode,output[-3000:])
  assert termios.tcgetattr(slave)==old
  failures=[e for e in map(json.loads,events.read_text().splitlines()) if e['Action']=='fail' and 'Test' not in e]
  assert len(failures)==2,failures
 finally:
  if p.poll() is None:p.kill();p.wait()
  os.close(master);os.close(slave)
print('Multi-package tests passed')
