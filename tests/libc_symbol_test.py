"""Check runtime import spelling in real objects without a macOS SDK or runner."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

probe, clang = sys.argv[1:]
nm = shutil.which('llvm-nm')
assert nm, 'llvm-nm is required for the cross-target libc symbol regression'
names = {'malloc', 'calloc', 'free', 'memcmp', 'atexit', 'write', '_setjmp'}
with tempfile.TemporaryDirectory(prefix='ocean-libc-symbols-') as directory:
    root = Path(directory)
    for target, darwin in [('x86_64-apple-macosx11.0', True),
                           ('arm64-apple-macosx11.0', True),
                           ('x86_64-unknown-linux-gnu', False),
                           ('x86_64-w64-windows-gnu', False),
                           ('wasm32-unknown-unknown', False)]:
        ir = subprocess.check_output([probe, '--libc-symbols', 'darwin' if darwin else 'plain'], text=True)
        source, obj = root/'symbols.ll', root/'symbols.o'
        source.write_text(ir)
        subprocess.run([clang, '-target', target, '-Wno-override-module', '-c', str(source), '-o', str(obj)], check=True, timeout=30)
        symbols = subprocess.check_output([nm, '--undefined-only', str(obj)], text=True)
        actual = {line.split()[-1] for line in symbols.splitlines() if line.strip()}
        imports = names | {'_longjmp' if darwin else 'longjmp'}
        expected = {('_' if darwin else '') + name for name in imports}
        assert ('_longjmp' if darwin else '__longjmp') not in actual, (target, symbols)
        assert expected <= actual, (target, expected - actual, symbols)
        assert not ({name if darwin else '_' + name for name in imports} & actual), (target, symbols)
        print(target + ': libc imports match target ABI')
