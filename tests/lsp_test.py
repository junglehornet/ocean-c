"""CLI check mode and a real stdio LSP session, including editor overlays."""
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())


def frame(message):
    body = json.dumps(message, ensure_ascii=False).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


with tempfile.TemporaryDirectory(prefix='ocean lsp ') as directory:
    root = Path(directory)
    source = root / 'hello % "雪".oc'
    good = 'package main;\nfunc main() {}\n'
    bad = 'package main;\nfunc main() {\n x: int = false;\n}\n'
    source.write_text(good)
    artifact = root / 'a.ll'
    artifact.write_text('keep existing output')
    before = {p.name: p.read_bytes() for p in root.iterdir()}
    for location in ([], [str(source)], [str(root)]):
        checked = subprocess.run([compiler, 'check', *location], cwd=root,
                                 capture_output=True, timeout=10)
        assert checked.returncode == 0, checked.stderr
        assert not checked.stdout, checked.stdout
    assert {p.name: p.read_bytes() for p in root.iterdir()} == before
    source.write_text(bad)
    checked = subprocess.run([compiler, 'check', str(source)], capture_output=True, timeout=10)
    assert checked.returncode == 1 and b'semantic error' in checked.stderr
    assert artifact.read_text() == 'keep existing output'
    source.write_text(good)
    for args in (['check', '-o', 'oops'], ['check', '--emit-llvm'],
                 ['check', '--lsp', str(source)], ['check', str(source), str(source)]):
        assert subprocess.run([compiler, *args], cwd=root, capture_output=True,
                              timeout=10).returncode != 0, args

    server = subprocess.Popen([compiler, 'check', '--lsp'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    def send(method, params=None, request_id=None):
        message = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            message['params'] = params
        if request_id is not None:
            message['id'] = request_id
        server.stdin.write(frame(message))

    def receive():
        def byte():
            assert select.select([server.stdout], [], [], 10)[0], 'LSP response timed out'
            value = server.stdout.read(1)
            assert value, 'LSP closed stdout'
            return value
        header = b''
        while not header.endswith(b'\r\n\r\n'):
            header += byte()
        length = int(header.split(b':', 1)[1].strip())
        return json.loads(b''.join(byte() for _ in range(length)))

    def diagnostics(uri, version=None):
        message = receive()
        assert message['method'] == 'textDocument/publishDiagnostics', message
        params = message['params']
        assert params['uri'] == uri, params
        if version is not None:
            assert params['version'] == version, params
        return [d for d in params['diagnostics'] if d['severity'] == 1]

    uri = source.as_uri()
    try:
        send('unknown', request_id=0)
        assert receive()['error']['code'] == -32002
        # String IDs and byte lengths must survive non-ASCII JSON.
        send('initialize', {'capabilities': {}}, '雪')
        reply = receive()
        assert reply['id'] == '雪'
        assert reply['result']['capabilities']['textDocumentSync']['change'] == 1
        send('initialized', {})
        send('textDocument/didOpen', {'textDocument': {
            'uri': uri, 'languageId': 'ocean', 'version': 1, 'text': bad}})
        errors = diagnostics(uri, 1)
        assert errors and errors[0]['range']['start'] == {'line': 2, 'character': 1}, errors
        assert 'type mismatch' in errors[0]['message'], errors
        assert source.read_text() == good
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 2},
                                      'contentChanges': [{'text': good}]})
        assert not diagnostics(uri, 2)
        # A stale change cannot replace the current buffer.
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 1},
                                      'contentChanges': [{'text': bad}]})
        assert not diagnostics(uri, 2)
        send('textDocument/didSave', {'textDocument': {'uri': uri}})
        assert not diagnostics(uri, 2)
        # Scanner failures run in a child, so the server remains usable.
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 3},
                                      'contentChanges': [{'text': 'package main;\nfunc main() { "unterminated'}]})
        assert diagnostics(uri, 3)
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 4},
                                      'contentChanges': [{'text': good}]})
        assert not diagnostics(uri, 4)
        # Unsaved new files join their package without being written to disk.
        new_file = root / 'new.oc'
        send('textDocument/didOpen', {'textDocument': {
            'uri': new_file.as_uri(), 'version': 1,
            'text': 'package main;\nfunc helper() int { return false; }'}})
        assert not diagnostics(uri, 4)
        assert diagnostics(new_file.as_uri(), 1)
        assert not new_file.exists()
        send('textDocument/didClose', {'textDocument': {'uri': new_file.as_uri()}})
        assert not diagnostics(new_file.as_uri(), 1)
        assert not diagnostics(uri, 4)
        unicode_text = 'package main;\nfunc main() { s := "😀"; x: int = false; }\n'
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 5},
                                      'contentChanges': [{'text': unicode_text}]})
        errors = diagnostics(uri, 5)
        offset = unicode_text.splitlines()[1].index('x:')
        expected_column = len(unicode_text.splitlines()[1][:offset].encode('utf-16-le')) // 2
        assert errors and errors[0]['range']['start'] == {'line': 1, 'character': expected_column}, errors
        send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 6},
                                      'contentChanges': [{'text': good}]})
        assert not diagnostics(uri, 6)
        send('textDocument/didClose', {'textDocument': {'uri': uri}})
        assert not diagnostics(uri, 6)
        send('unimplemented', request_id=42)
        assert receive()['error']['code'] == -32601
        # Invalid JSON gets a protocol error, followed by a usable server.
        server.stdin.write(b'Content-Length: 1\r\n\r\n{')
        assert receive()['error']['code'] == -32700
        send('shutdown', request_id=2)
        assert receive() == {'jsonrpc': '2.0', 'id': 2, 'result': None}
        send('exit')
        assert server.wait(timeout=10) == 0
        assert not server.stderr.read()
    finally:
        if server.poll() is None:
            server.kill()
        server.communicate(timeout=10)
    assert source.read_text() == good
    assert artifact.read_text() == 'keep existing output'

# Exiting without shutdown must fail, and no messages belong on stdout.
exited = subprocess.run([compiler, 'check', '--lsp'], input=frame(
    {'jsonrpc': '2.0', 'method': 'exit'}), capture_output=True, timeout=10)
assert exited.returncode == 1 and not exited.stdout
print('check CLI and LSP integration passed')
