"""Offline integration fixtures; the installer executable itself never uses Python."""
import argparse
import contextlib
import datetime
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import threading

parser=argparse.ArgumentParser()
parser.add_argument('installer',type=Path)
args=parser.parse_args()
installer=args.installer.resolve()
target=subprocess.check_output([str(installer),'--platform'],text=True).strip()
assert target in ('x86_64-linux','aarch64-linux','riscv64-linux'), target
def run(*args,**kw):
    return subprocess.check_output(args,text=True,stderr=subprocess.DEVNULL,**kw).strip()
with tempfile.TemporaryDirectory(prefix='ocean-native-installer-test-') as directory:
    root=Path(directory).resolve();home=root/'home';home.mkdir();temp=root/'temp';temp.mkdir()
    repos={}
    for name in ['ocean-c','ocean-src']:
        repo=root/name;run('git','init','-b','main',str(repo));repos[name]=repo
        run('git','-C',str(repo),'config','user.name','Installer Fixture')
        run('git','-C',str(repo),'config','user.email','fixture@example.invalid')
        (repo/'sample.oc').write_text('package sample;\n')
        run('git','-C',str(repo),'add','.')
        run('git','-C',str(repo),'commit','-m','Fixture')
        for tag in ['0.5.0','1.0.0','2.0.0-beta']:
            run('git','-C',str(repo),'tag','-a',tag,'-m',tag)
    compiler=repos['ocean-c'];commit=run('git','-C',str(compiler),'rev-parse','HEAD');date=run('git','-C',str(compiler),'show','-s','--format=%cs','HEAD')
    run('git','-C',str(compiler),'checkout','-b','stable')
    (compiler/'stable-only').write_text('x');run('git','-C',str(compiler),'add','.');run('git','-C',str(compiler),'commit','-m','Stable only')
    run('git','-C',str(compiler),'tag','-f','-a','1.0.0','-m','Stable tag')
    commit=run('git','-C',str(compiler),'rev-parse','HEAD')
    date=run('git','-C',str(compiler),'show','-s','--format=%cs','HEAD')
    run('git','-C',str(compiler),'checkout','main')
    run('git','-C',str(compiler),'checkout','-b','nightly')
    (compiler/'nightly-only').write_text('x');run('git','-C',str(compiler),'add','.');run('git','-C',str(compiler),'commit','-m','Nightly only')
    run('git','-C',str(compiler),'tag','-f','-a','2.0.0-beta','-m','Nightly tag')
    run('git','-C',str(compiler),'checkout','main')
    (compiler/'main-only').write_text('x');run('git','-C',str(compiler),'add','.');run('git','-C',str(compiler),'commit','-m','Main only');run('git','-C',str(compiler),'tag','9.0.0')
    binary=root/'fixture-compiler'
    # A real native fixture, so there is no Python/shell fallback in the installed compiler.
    code=root/'fixture.c';code.write_text('#include <stdio.h>\nint main(void){puts("Ocean compiler (ocean-c) 1.0.0\\nCommit '+commit+' ('+date+'), built 2026-11-13T12:00:00Z\\nCopyright Connor Egan '+date[:4]+'");}\n')
    subprocess.run(['clang',str(code),'-o',str(binary)],check=True)
    nightlyCommit=run('git','-C',str(compiler),'rev-parse','nightly')
    nightlyCode=root/'nightly.c';nightlyCode.write_text(code.read_text().replace('1.0.0','2.0.0-beta').replace(commit,nightlyCommit))
    nightlyBinary=root/'fixture-nightly';subprocess.run(['clang',str(nightlyCode),'-o',str(nightlyBinary)],check=True)
    def archive(name='ocean-c', binary=binary):
        stream=io.BytesIO()
        with tarfile.open(fileobj=stream,mode='w',format=tarfile.PAX_FORMAT) as tar:
            tar.add(binary,arcname=name)
        return stream.getvalue()
    clang=root/'fixture-clang';clangSource=root/'clang.c';clangSource.write_text('#include <stdio.h>\nint main(void){puts("clang version fixture");}\n')
    subprocess.run(['clang',str(clangSource),'-o',str(clang)],check=True)
    bundle=io.BytesIO()
    with tarfile.open(fileobj=bundle,mode='w:xz') as tar:tar.add(clang,arcname='bin/clang')
    state={'body':archive(),'deps':bundle.getvalue(),'fail':False,'redirects':0}
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            if self.path.startswith('/api/repos/'):
                releases=[]
                for tag,pre in [('2.0.0-beta',True),('1.0.0',False)]:
                    releases.append(dict(tag_name=tag,prerelease=pre,draft=False,assets=[dict(name=f'ocean-c_{tag}_{target}.tar',browser_download_url=f'http://127.0.0.1:{self.server.server_port}/redirect',digest='sha256:'+hashlib.sha256(state['body']).hexdigest()),dict(name=f'ocean-deps_{tag}_{target}.tar.xz',browser_download_url=f'http://127.0.0.1:{self.server.server_port}/deps.tar.xz',digest='sha256:'+hashlib.sha256(state['deps']).hexdigest())]))
                data=json.dumps(releases).encode();self.send_response(200)
            elif self.path=='/redirect':
                state['redirects']+=1;self.send_response(302);self.send_header('Location','/compiler.tar');self.send_header('Content-Length','0');self.end_headers();return
            elif self.path=='/deps.tar.xz':
                data=state['deps'];self.send_response(200)
            elif self.path=='/compiler.tar':
                data=state['body'];self.send_response(503 if state['fail'] else 200)
            else:data=b'not found';self.send_response(404)
            self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    with http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        env=dict(os.environ,HOME=str(home),TMPDIR=str(temp),GITHUB_TOKEN='',OCEAN_INSTALLER_API_BASE=f'http://127.0.0.1:{server.server_port}/api',GIT_CONFIG_COUNT='2')
        for i,name in enumerate(['ocean-c','ocean-src']):
            env[f'GIT_CONFIG_KEY_{i}']=f'url.{repos[name]}.insteadOf';env[f'GIT_CONFIG_VALUE_{i}']=f'https://github.com/junglehornet/{name}.git'
        destination=home/'.local/lib/ocean'
        def invoke(input,success=False):
            result=subprocess.run([str(installer)],input=input,text=True,capture_output=True,env=env,timeout=120)
            assert (result.returncode==0)==success,(result.returncode,result.stdout,result.stderr)
            assert list(temp.iterdir())==[],list(temp.iterdir())
            return result
        result=invoke('2\ny\n1\nlatest\n',True)
        assert '9.0.0' not in result.stdout,result.stdout
        assert '2.0.0-beta' in result.stdout,result.stdout
        assert result.stdout.count('. 1.0.0')==1,result.stdout
        assert result.stdout.count('. 0.5.0')==1,result.stdout
        assert 'stable or nightly' in result.stdout,result.stdout
        assert state['redirects']>0
        metadata=json.loads((destination/'installation.json').read_text())
        assert metadata['version']=='1.0.0' and metadata['commit']==commit,metadata
        assert metadata['build_date']=='2026-11-13T12:00:00Z',metadata
        assert (destination/'src/sample.oc').is_file() and not (destination/'src/.git').exists()
        assert 'Ocean compiler' in run(str(destination/'bin/ocean'),'--version')
        profiles={p:p.read_bytes() for p in home.glob('.*') if p.is_file()}
        result=invoke('2\n\n');assert '1.0.0' in result.stdout and 'install_date' in result.stdout
        assert profiles=={p:p.read_bytes() for p in profiles}
        before=(destination/'installation.json').read_bytes()
        prefix=f'2\nremove {destination}\ny\n1\nlatest\n'
        state['fail']=True;invoke(prefix);assert before==(destination/'installation.json').read_bytes()
        state['fail']=False;state['body']=archive('../escape');invoke(prefix)
        assert before==(destination/'installation.json').read_bytes() and not (root/'escape').exists()
        state['body']=archive();invoke(prefix,True)
        assert profiles=={p:p.read_bytes() for p in profiles},'PATH block duplicated'
        invoke(f'2\nremove {destination}\ny\n3\nlatest\n',True)
        assert (destination/'bin/clang').is_file()
        assert json.loads((destination/'installation.json').read_text())['dependencies']=='bundle'
        # Declining a system package operation never invokes a real package manager.
        invoke(f'2\nremove {destination}\ny\n2\nn\n')
        state['body']=archive(binary=nightlyBinary)
        invoke(f'2\nremove {destination}\ny\n1\nlatest-nightly\n',True)
        metadata=json.loads((destination/'installation.json').read_text())
        assert metadata['version']=='2.0.0-beta' and metadata['commit']==nightlyCommit,metadata
        (destination/'installation.json').write_text('{')
        result=invoke('2\n\n');assert 'Unknown version' in result.stdout
        invoke('')
        server.shutdown();thread.join()
print('Native installer integration fixtures passed.')
