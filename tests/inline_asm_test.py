"""Inline assembly selectors, raw bodies, diagnostics, and register preservation."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
architecture = {"AMD64": "x86_64", "arm64": "aarch64"}.get(platform.machine(), platform.machine())
os_name = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}[platform.system()]


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean asm ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("main.oc", "main.ll", "native"))

    def build(text, ok=True):
        source.write_text("package main; " + text)
        result = run([compiler, "check", str(source)])
        assert (result.returncode == 0) == ok, (text, result.stdout, result.stderr)
        return result.stdout + result.stderr

    # All known selectors parse. Invalid foreign assembly is never assembled.
    blocks = []
    for arch in ("x86_64", "aarch64", "riscv64"):
        for os in (None, "linux", "macos", "windows"):
            matches = arch == architecture and (os is None or os == os_name)
            body = 'nop' if matches else 'not_an_instruction @ $foreign %reg #123\n'
            blocks.append(f'asm {arch}' + (f'/{os}' if os else '') + ' {' + body + '}')
    build('func main() { ' + '\n'.join(blocks) + ' }')

    for text, message in (
        ('func main() { asm linux { nop } }', 'Unknown asm architecture'),
        ('func main() { asm x86 { nop } }', 'Unknown asm architecture'),
        ('func main() { asm x86_64/freebsd { nop } }', 'Unknown asm platform'),
        ('func main() { asm x86_64/ { nop } }', 'Expected asm platform'),
        ('func main() { asm { nop } }', 'Expected asm architecture'),
        ('func main() { asm x86_64 nop; }', 'required'),
        ('func main() { asm x86_64 { nop', 'Unterminated asm block'),
        ('asm x86_64 { nop } func main() {}', 'only allowed inside functions'),
    ):
        assert message in build(text, False), text

    if architecture in ("x86_64", "aarch64", "riscv64"):
        build(f'func main() {{ asm {architecture} {{ not_an_instruction }} }}', False)
        body = {
            'x86_64': 'mov rax, 99\nxor rbx, rbx\nxor r12, r12\npxor xmm0, xmm0\n',
            'aarch64': 'mov x0, #99\nmov x19, #0\nmovi v0.16b, #0\n',
            'riscv64': 'li a0, 99\nli s1, 0\n',
        }[architecture]
        comment = '// ignored brace }\n' if architecture == 'aarch64' else '# ignored brace }\n'
        # Repeated numeric labels are local to their assembly locations.
        body += '1:\nnop\n' + comment
        text = f'''func identity<T>(v T) T {{ asm {architecture} {{ {body} }} return v; }}
func main() int {{ a := 7; f := 2.5;
    asm {architecture}/{os_name} {{ {body} }};
    asm {architecture} {{ .ascii "" // quoted braces: {{ }}
    }}
    if (f != 2.5) return 1;
    return identity<int>(a);
}}'''
        build(text)
        emitted = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        assert 'asm sideeffect' in ir.read_text()
        for optimization in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', optimization, '-x', 'ir', str(ir), '-o', str(executable)])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)])
            assert result.returncode == 7, result
        if architecture == 'x86_64' and os_name == 'linux':
            # An observable syscall proves matching assembly executes, including at -O2.
            build('''func main() int { asm x86_64/linux {
                mov eax, 1
                mov edi, 1
                lea rsi, [rip + 1f]
                mov edx, 3
                syscall
                jmp 2f
                1: .ascii "asm"
                2:
            } return 0; }''')
            emitted = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
            assert emitted.returncode == 0, emitted.stdout + emitted.stderr
            for optimization in ('-O0', '-O2'):
                result = run([clang, '-Wno-override-module', optimization, '-x', 'ir', str(ir), '-o', str(executable)])
                assert result.returncode == 0, result.stderr
                result = run([str(executable)])
                assert result.returncode == 0 and result.stdout == 'asm', result
        # Assembler strings must survive both LLVM string escaping and template expansion.
        build(f'func main() {{ asm {architecture} {{ jmp 1f\n.ascii "$ {{ }} \\\\"\n1:\nnop\n }} }}'
              if architecture == 'x86_64' else f'func main() {{ asm {architecture} {{ nop }} }}')

    for declaration, message in (
        ('', 'unresolved asm operand'),
        ('missing := "text";', 'mutable scalar'),
        ('missing := int[]{1};', 'mutable scalar'),
    ):
        diagnostic = build('func main() { ' + declaration +
                           ' asm x86_64 { mov rax, ${missing} } }', False)
        assert message in diagnostic, diagnostic
    for limited_arch in ('aarch64', 'riscv64'):
        diagnostic = build('func main() { a:=0; b:=0; c:=0; d:=0; e:=0; '
                           f'asm {limited_arch} {{ ${{a}} ${{b}} ${{c}} ${{d}} ${{e}} }} }}', False)
        assert 'at most four' in diagnostic, diagnostic
        instruction = 'ldr x0, ' if limited_arch == 'aarch64' else 'ld a0, '
        body = '\n'.join(instruction + '${' + name + '}' for name in ('a', 'b', 'c', 'd', 'a'))
        build('func main() { a:=0; b:=0; c:=0; d:=0; '
              f'asm {limited_arch} {{\n{body}\n}} }}')

    # Exercise dynamic operand storage, two-digit LLVM indices and repeated refs.
    names = [f'v{i}' for i in range(16)]
    declarations = ' '.join(f'{name} := {i + 1};' for i, name in enumerate(names))
    assembly = '\n'.join(f'mov rax, ${{{name}}}\nadd rax, 1\nmov ${{{name}}}, rax'
                         for name in names)
    build('func main() int { ' + declarations + ' asm x86_64 {\n' + assembly +
          '\n} return ' + ' + '.join(names) + '; }')
    if architecture == 'x86_64':
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
        assert result.returncode == 0, result.stdout + result.stderr
        for optimization in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', optimization, '-x', 'ir', str(ir), '-o', str(executable)])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)])
            assert result.returncode == 152, result
    if architecture in ('x86_64', 'aarch64', 'riscv64'):
        body = {
            'x86_64': 'mov rax, ${value}\nadd rax, ${delta}\nmov ${value}, rax',
            'aarch64': 'ldr x0, ${value}\nldr x1, ${delta}\nadd x0, x0, x1\nstr x0, ${value}',
            'riscv64': 'ld a0, ${value}\nld a1, ${delta}\nadd a0, a0, a1\nsd a0, ${value}',
        }[architecture]
        # Global qualification, local shadowing and repeated read/write references.
        build('value := 7; func main() int { delta := 3; { delta := 99; } '
              f'asm {architecture} {{ {body} }} return value; }}')
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
        assert result.returncode == 0, result.stdout + result.stderr
        for optimization in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', optimization, '-x', 'ir', str(ir), '-o', str(executable)])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)])
            assert result.returncode == 10, result

print('inline assembly tests passed')
