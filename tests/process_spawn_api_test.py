"""Exercise old/new Darwin SDK and runtime API selection without a macOS host."""
from pathlib import Path
import subprocess
import sys
import tempfile

clang = sys.argv[1]
runtime = Path(__file__).resolve().parents[1] / 'runtime_process.c'
with tempfile.TemporaryDirectory(prefix='ocean-spawn-api-') as directory:
    root = Path(directory)
    (root/'crt_externs.h').write_text('extern char **environ;\nstatic char ***_NSGetEnviron(void) { return &environ; }\n')
    (root/'spawn.h').write_text('''#include_next <spawn.h>
int test_modern(posix_spawn_file_actions_t *, const char *);
int test_legacy(posix_spawn_file_actions_t *, const char *) __attribute__((deprecated));
''')
    source = root/'test.c'
    source.write_text('''#define __APPLE__ 1
static int available, modern_calls, legacy_calls;
#define __builtin_available(...) available
#define posix_spawn_file_actions_addchdir test_modern
#define posix_spawn_file_actions_addchdir_np test_legacy
#include "''' + str(runtime).replace('\\', '\\\\') + '''"
int test_modern(posix_spawn_file_actions_t *a, const char *p) { (void)a; (void)p; modern_calls++; return 19; }
int test_legacy(posix_spawn_file_actions_t *a, const char *p) { (void)a; (void)p; legacy_calls++; return 23; }
int main(void) {
    available = 0;
    if (add_directory(0, "/") != 23 || legacy_calls != 1 || modern_calls) return 1;
    available = 1;
#ifdef __MAC_26_0
    if (add_directory(0, "/") != 19 || modern_calls != 1 || legacy_calls != 1) return 2;
#else
    if (add_directory(0, "/") != 23 || modern_calls || legacy_calls != 2) return 3;
#endif
    return 0;
}
''')
    for modern_sdk in (False, True):
        (root/'Availability.h').write_text('#define __MAC_26_0 260000\n#define __MAC_OS_X_VERSION_MAX_ALLOWED 260000\n' if modern_sdk else '#undef __MAC_26_0\n#undef __MAC_OS_X_VERSION_MAX_ALLOWED\n')
        binary = root/'test'
        subprocess.run([clang, '-std=c11', '-Werror=deprecated-declarations', '-I'+str(root), str(source), '-o', str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=10)
print('Darwin spawn APIs: old SDK, old runtime, and macOS 26 paths passed')
