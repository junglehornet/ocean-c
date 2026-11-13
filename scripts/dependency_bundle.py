"""Collect a relocatable native Clang, C sysroot, libffi and libcurl release bundle."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


def output(*args):
    return subprocess.check_output(args,text=True).strip()


def copy(source, target):
    source, target = Path(source), Path(target)
    target.parent.mkdir(parents=True,exist_ok=True)
    if source.is_dir():
        shutil.copytree(source,target,dirs_exist_ok=True,symlinks=True)
    else:
        # Resolve links so no absolute package-manager symlinks escape the bundle.
        shutil.copy2(source.resolve(),target)


def build_bundle(destination, version, arch, system):
    with tempfile.TemporaryDirectory(prefix='ocean-dependencies-') as temporary:
        root=Path(temporary)/'bundle';(root/'bin').mkdir(parents=True);(root/'lib').mkdir()
        clang=Path(shutil.which('clang')).resolve()
        resource=Path(output(str(clang),'-print-resource-dir'))
        copy(resource,root/'lib/clang'/resource.name)
        if system=='windows':
            prefix=Path(os.environ['MINGW_PREFIX'])
            # MSYS2 exposes a Unix-style prefix to its shell but Python is native.
            if not prefix.exists(): prefix=Path(output('cygpath','-m',str(prefix)))
            for name in ('clang.exe','ld.lld.exe','lld.exe','llvm-ar.exe','llvm-ranlib.exe'):
                file=prefix/'bin'/name
                if file.exists(): copy(file,root/'bin'/name)
            for file in (prefix/'bin').glob('*.dll'): copy(file,root/'bin'/file.name)
            copy(prefix/'include',root/'include')
            for file in (prefix/'lib').iterdir():
                if file.is_file() and not file.name.startswith(('libLLVM','libclang','libpython')) and file.suffix in ('.a','.o'):
                    copy(file,root/'lib'/file.name)
            triple='aarch64-w64-windows-gnu' if arch=='aarch64' else 'x86_64-w64-windows-gnu'
            (root/'bin/clang.cfg').write_text(f'--target={triple}\n--sysroot=<CFGDIR>/..\n-fuse-ld=lld\n-I<CFGDIR>/../include\n-L<CFGDIR>/../lib\n')
        elif system=='linux':
            for name in ('clang','ld.lld','llvm-ar','llvm-ranlib'):
                located=shutil.which(name) or str(clang.parent/name)
                if not Path(located).exists(): raise RuntimeError(f'Missing dependency tool: {name}')
                copy(located,root/'bin'/name)
            copy('/usr/include',root/'sysroot/usr/include')
            # GCC CRT objects and static support libraries retain their native layout.
            if Path('/usr/lib/gcc').exists(): copy('/usr/lib/gcc',root/'sysroot/usr/lib/gcc')
            multiarch=output(str(clang),'-print-target-triple')
            machine=output('gcc','-print-multiarch')
            libdir=Path('/usr/lib')/machine; target=root/'sysroot/usr/lib'/machine
            core=re.compile(r'^(?:[Sgr]?crt.*\.o|lib(?:c|m|pthread|dl|rt|util|resolv|gcc_s|stdc\+\+)(?:[._-].*)?|ld-linux.*)$')
            for file in libdir.iterdir():
                if file.is_file() and core.match(file.name): copy(file,target/file.name)
            runtime=Path('/lib')/machine
            if runtime.exists():
                for file in runtime.iterdir():
                    if file.is_file() and core.match(file.name): copy(file,root/'sysroot/lib'/machine/file.name)
            # Shared dependencies, including developer link names, are local to lib/.
            native_libraries=set()
            for name in ('libffi','libcurl'):
                directory=Path(output('pkg-config','--variable=libdir',name))
                pattern='libffi.so*' if name=='libffi' else 'libcurl.so*'
                for file in directory.glob(pattern): copy(file,root/'lib'/file.name);native_libraries.add(file.resolve())
            for name in ('libffi','libcurl'):
                directory=Path(output('pkg-config','--variable=includedir',name))
                if name=='libcurl' and (directory/'curl').exists(): copy(directory/'curl',root/'include/curl')
                if name=='libffi':
                    for file in directory.glob('ffi*.h'): copy(file,root/'include'/file.name)
            seen=set();queue=list((root/'bin').iterdir())+list((root/'lib').glob('*.so*'))
            while queue:
                binary=queue.pop()
                if binary in seen: continue
                seen.add(binary)
                result=subprocess.run(['ldd',str(binary)],text=True,capture_output=True)
                for dependency in re.findall(r'=> (/\S+)',result.stdout):
                    file=Path(dependency)
                    if re.match(r'lib(?:c|m|pthread|dl|rt|resolv)\.so',file.name) or file.name.startswith('ld-linux'): continue
                    native_libraries.add(file.resolve())
                    target=root/'lib'/file.name
                    if not target.exists(): copy(file,target);queue.append(target)
                subprocess.run(['patchelf','--set-rpath','$ORIGIN/../lib' if binary.parent.name=='bin' else '$ORIGIN',str(binary)],check=True)
            (root/'bin/clang.cfg').write_text('--sysroot=<CFGDIR>/../sysroot\n--gcc-toolchain=<CFGDIR>/../sysroot/usr\n-fuse-ld=lld\n-I<CFGDIR>/../include\n-L<CFGDIR>/../lib\n-Wl,-rpath,<CFGDIR>/../lib\n')
        else:
            for name in ('clang','ld.lld','llvm-ar','llvm-ranlib'): copy(shutil.which(name) or clang.parent/name,root/'bin'/name)
            # macOS provides libcurl and its native trust store with the OS SDK.
            for package, pattern in [('libffi','libffi*.dylib')]:
                prefix=Path(output('brew','--prefix',package))
                for file in (prefix/'lib').glob(pattern): copy(file,root/'lib'/file.name)
                copy(prefix/'include',root/'include')
            brewroot=Path(output('brew','--prefix'))
            queue=list((root/'bin').iterdir())+list((root/'lib').iterdir());seen=set()
            while queue:
                file=queue.pop()
                if file in seen:continue
                seen.add(file)
                for line in output('otool','-L',str(file)).splitlines()[1:]:
                    linked=line.strip().split(' (',1)[0]
                    if linked.startswith(('/usr/lib/','/System/')):continue
                    source=Path(linked)
                    if linked.startswith('@'):
                        basename=Path(linked).name
                        candidates=[clang.parent.parent/'lib'/basename,*list((brewroot/'opt').glob('*/lib/'+basename))]
                        source=next((p for p in candidates if p.exists()),None)
                        if source is None: raise RuntimeError(f'Cannot relocate {linked}')
                    target=root/'lib'/source.name
                    if not target.exists(): copy(source,target);queue.append(target)
                    subprocess.run(['install_name_tool','-change',linked,'@rpath/'+target.name,str(file)],check=True)
                rpath='@executable_path/../lib' if file.parent.name=='bin' else '@loader_path'
                if rpath not in output('otool','-l',str(file)):
                    subprocess.run(['install_name_tool','-add_rpath',rpath,str(file)],check=True)
                subprocess.run(['codesign','--force','--sign','-',str(file)],check=True)
            (root/'bin/clang.cfg').write_text('-I<CFGDIR>/../include\n-L<CFGDIR>/../lib\n-Wl,-rpath,<CFGDIR>/../lib\n')
        licenses=root/'share/ocean-dependencies';licenses.mkdir(parents=True)
        (licenses/'README.txt').write_text('Native tools/libraries retain their upstream licenses. See included package copyright notices. Clang/LLVM: Apache-2.0 WITH LLVM-exception; libffi: MIT; libcurl: curl license.\n')
        if system=='linux':
            for library in native_libraries if shutil.which('dpkg-query') else []:
                owned=subprocess.run(['dpkg-query','-S',str(library)],capture_output=True,text=True)
                for line in owned.stdout.splitlines():
                    package=line.split(': ',1)[0].split(':',1)[0]
                    notice=Path('/usr/share/doc')/package/'copyright'
                    if notice.exists():copy(notice,licenses/(package+'-copyright'))
            for package in ('clang-19','llvm-19','libffi8','libcurl4t64','libc6-dev','gcc','libssl3t64','libpsl5t64','libidn2-0','zlib1g'):
                file=Path('/usr/share/doc')/package/'copyright'
                if file.exists():copy(file,licenses/(package+'-copyright'))
        else:
            prefixes=[clang.parent.parent]
            if system=='macos':prefixes.extend(Path(output('brew','--prefix',p)) for p in ('libffi','curl'))
            for number,prefix in enumerate(prefixes):
                for directory in ('share/licenses','share/doc/llvm','LICENSE.TXT'):
                    file=prefix/directory
                    if file.exists():copy(file,licenses/str(number)/directory.replace('/','_'))
        (licenses/'build.json').write_text(json.dumps(dict(version=version,arch=arch,os=system),indent=2)+'\n')
        # Clang must run from the relocated bundle before it can be published.
        executable=root/'bin'/('clang.exe' if system=='windows' else 'clang')
        subprocess.run([str(executable),'--version'],check=True)
        probe=Path(temporary)/'probe.c';probe.write_text('#include <stdio.h>\n#include <ffi.h>\n#include <curl/curl.h>\nint main(void){if(!curl_version() || !sizeof(ffi_cif))return 1;puts("bundle-ok");return 0;}\n')
        application=Path(temporary)/('probe.exe' if system=='windows' else 'probe')
        config=root/'bin/clang.cfg';base=config.read_text()
        if system=='macos':config.write_text('-isysroot\n"'+output('xcrun','--show-sdk-path')+'"\n'+base)
        subprocess.run([str(executable),str(probe),'-lffi','-lcurl','-o',str(application)],check=True)
        env=dict(os.environ,PATH=str(root/'bin')+os.pathsep+os.environ['PATH'])
        assert subprocess.check_output([str(application)],text=True,env=env).strip()=='bundle-ok'
        config.write_text(base)
        destination=Path(destination);destination.mkdir(parents=True,exist_ok=True)
        archive=destination/f'ocean-deps_{version}_{arch}-{system}.tar.xz'
        with tarfile.open(archive,'w:xz') as tar:
            for file in sorted(root.iterdir()):tar.add(file,arcname=file.name)
        return archive
