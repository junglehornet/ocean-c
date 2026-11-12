"""Semantic LSP hover, navigation, references, rename and contextual completion."""
from pathlib import Path
import json, os, select, subprocess, sys, tempfile
compiler=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='ocean semantic lsp ') as d:
    root=Path(d);source=root/'main.oc';sibling=root/'other.oc'
    standard=root/'std';standard.mkdir();(standard/'ocean.mod').write_text('module std\nocean 0.1\n')
    (standard/'formatting').mkdir();(standard/'formatting'/'pretty.oc').write_text('/* header */ package pretty; func Print() {}')
    text='''package main;
struct Person { Name string; Age int; }
func (p Person) Read() int { return p.Age; }
func main() int {
    person := Person{Name: "Ada", Age: 36};
    number := 1;
    { number := "inner"; person.Name = number; }
    result := number + helper();
    return result + person.Read();
}
'''
    source.write_text(text);sibling.write_text('package main; func helper() int { return 2; }')
    server=subprocess.Popen([compiler,'check','--lsp'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,env={**os.environ,'OCEAN_SRC':str(standard)})
    seq=0
    def send(method,params=None,id=None):
        msg={'jsonrpc':'2.0','method':method}
        if params is not None:msg['params']=params
        if id is not None:msg['id']=id
        data=json.dumps(msg).encode();server.stdin.write(f'Content-Length: {len(data)}\r\n\r\n'.encode()+data)
    def recv():
        def byte():
            assert select.select([server.stdout],[],[],20)[0],'timeout'
            x=server.stdout.read(1);assert x,'closed';return x
        h=b''
        while not h.endswith(b'\r\n\r\n'):h+=byte()
        n=int(h.split(b':',1)[1].strip());return json.loads(b''.join(byte() for _ in range(n)))
    def request(method,line,col,extra=None):
        global seq
        seq+=1;params={'textDocument':{'uri':source.as_uri()},'position':{'line':line,'character':col}}
        if extra:params.update(extra)
        send(method,params,seq);r=recv();assert r.get('id')==seq,r;return r
    def at(line,word,occurrence=0):
        s=text.splitlines()[line];p=-1
        for _ in range(occurrence+1):p=s.index(word,p+1)
        return p
    try:
        send('initialize',{'capabilities':{}},1);r=recv();caps=r['result']['capabilities'];assert all(caps[x] for x in ['hoverProvider','definitionProvider','referencesProvider','renameProvider','completionProvider'])
        send('textDocument/didOpen',{'textDocument':{'uri':source.as_uri(),'version':1,'text':text}});assert recv()['method']=='textDocument/publishDiagnostics'
        r=request('textDocument/hover',7,at(7,'number')+1);assert 'int' in r['result']['contents']['value'],r
        r=request('textDocument/hover',6,at(6,'number',1)+1);assert 'string' in r['result']['contents']['value'],r
        r=request('textDocument/definition',7,at(7,'number')+1);assert r['result'][0]['range']['start']['line']==5,r
        r=request('textDocument/definition',7,at(7,'helper')+1);assert r['result'][0]['uri']==sibling.as_uri(),r
        r=request('textDocument/definition',4,at(4,'Name:')+1);assert r['result'][0]['range']['start']['line']==1,r
        r=request('textDocument/typeDefinition',8,at(8,'person')+1);assert r['result'][0]['range']['start']['line']==1,r
        r=request('textDocument/rename',5,at(5,'number')+1,{'newName':'count'});edits=r['result']['changes'][source.as_uri()];assert len(edits)==2 and {e['range']['start']['line'] for e in edits}=={5,7},r
        r=request('textDocument/rename',1,at(1,'Name')+1,{'newName':'Label'});edits=r['result']['changes'][source.as_uri()];assert len(edits)==3,r
        r=request('textDocument/rename',5,at(5,'number')+1,{'newName':'person'});assert r.get('error'),r
        r=request('textDocument/rename',5,at(5,'number')+1,{'newName':'for'});assert r.get('error'),r
        r=request('textDocument/references',7,at(7,'helper')+1,{'context':{'includeDeclaration':True}});assert len(r['result'])==2,r
        # Completion uses the current unsaved expression and inferred receiver type.
        edited=text.replace('return result + person.Read();','person.\n    return result;')
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':2},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',8,len('    person.'));labels={x['label'] for x in r['result']['items']};assert {'Name','Age','Read'}<=labels,r
        edited=text.replace('return result + person.Read();','numb\n    return result;')
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':3},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',8,len('    numb'));assert [x['label'] for x in r['result']['items']]==['number'],r
        # Imported definitions, visibility-aware completion, and rename edits.
        (root/'ocean.mod').write_text('module example.com/lsp\nocean 0.1\n')
        lib=root/'lib';lib.mkdir();library=lib/'lib.oc'
        library.write_text('package lib; struct Thing { Field int; } func Make() Thing { return Thing{Field: 1}; } func hidden() {}')
        edited='package main;\nimport "example.com/lsp/lib";\nfunc main() int { thing := lib.Make(); return thing.Field; }\n'
        source.write_text(edited)
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':4},'contentChanges':[{'text':edited}]});recv()
        col=edited.splitlines()[2].index('Make')+1
        r=request('textDocument/definition',2,col);assert r['result'][0]['uri']==library.as_uri(),r
        r=request('textDocument/rename',2,col,{'newName':'Create'});assert set(r['result']['changes'])=={source.as_uri(),library.as_uri()},r
        r=request('textDocument/rename',2,col,{'newName':'create'});assert r.get('error'),r
        edited='package main;\nimport "example.com/lsp/lib";\nfunc main() { lib. }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':5},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',2,len('func main() { lib.'));labels={x['label'] for x in r['result']['items']};assert {'Make','Thing'}<=labels and 'hidden' not in labels,r
        edited='package main;\nimport "example.com/lsp/lib";\nfunc main() { lib.Make(). }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':6},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',2,len('func main() { lib.Make().'));assert 'Field' in {x['label'] for x in r['result']['items']},r
        edited='package main;\nfunc main() int { café := 1; /* 🌊 */ return café; }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':7},'contentChanges':[{'text':edited}]});recv()
        line=edited.splitlines()[1];pos=line.rindex('café');col=len(line[:pos].encode('utf-16-le'))//2
        r=request('textDocument/hover',1,col+1);assert 'int' in r['result']['contents']['value'],r
        assert r['result']['range']['start']['character']==col,r
        r=request('textDocument/rename',1,col+1,{'newName':'数'});assert len(r['result']['changes'][source.as_uri()])==2,r
        edited='package main;\nfunc main() { for (idx := 0; idx < 1; idx++) {} id }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':8},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',1,len('func main() { for (idx := 0; idx < 1; idx++) {} id'));assert 'idx' not in {x['label'] for x in r['result']['items']},r
        edited='package main;\nfunc main() { defer main(); }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':9},'contentChanges':[{'text':edited}]})
        r=recv();assert not r['params']['diagnostics'],r
        edited='package main;\nfunc main() int { result := Result<int>.value(7); return result.unwrap(); }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':10},'contentChanges':[{'text':edited}]});r=recv()
        assert not r['params']['diagnostics'],r
        col=edited.splitlines()[1].index('unwrap')+1
        r=request('textDocument/hover',1,col);assert 'int' in r['result']['contents']['value'],r
        r=request('textDocument/definition',1,col);assert r['result']==[],r
        r=request('textDocument/prepareRename',1,col);assert r['result'] is None,r
        r=request('textDocument/rename',1,col,{'newName':'take'});assert r.get('error'),r
        edited='package main;\nfunc main() { result := Result<int>.value(7); result. }\n'
        send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':11},'contentChanges':[{'text':edited}]});recv()
        r=request('textDocument/completion',1,len('func main() { result := Result<int>.value(7); result.'))
        labels={item['label'] for item in r['result']['items']}
        assert {'unwrap','unwrapOrElse','unwrapOrZero'}<=labels,r
        # Exercise complete tokens, empty scope, parameters, shadowing, and enums.
        version = 11
        def complete(buffer):
            global version, last_diagnostics
            version += 1
            before, after = buffer.split('|')
            edited = before + after
            send('textDocument/didChange', {'textDocument': {'uri': source.as_uri(), 'version': version},
                                          'contentChanges': [{'text': edited}]})
            last_diagnostics = recv()['params']['diagnostics']
            line = before.count('\n')
            col = len(before.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2
            result = request('textDocument/completion', line, col)['result']
            return {item['label']: item for item in result['items']}
        items = complete('package main; func main() { local := 1; lo|cal; }')
        assert 'local' in items, items
        items = complete('package main; func use(param int) { local := 1; | } func main() {}')
        assert {'param', 'local', 'use', 'helper', 'main', 'make', 'zero'} <= items.keys(), items
        items = complete('package main; func main() { before := 1; { hidden := 2; } | later := 3; }')
        assert 'before' in items and not {'hidden', 'later'} & items.keys(), items
        items = complete('package main; func main() { value := 1; { value := "x"; val| } }')
        assert items['value']['detail'] == 'string', items
        items = complete('package main; enum Color { Red, Green, RGB(int, int, int) } func main() { Color.| }')
        assert {'Red', 'Green', 'RGB'} <= items.keys(), items
        assert items['Red']['kind'] == 20, items
        items = complete('package main; func main() { Optional<int>.| }')
        labels = set(items)
        assert {'value', 'none'} <= labels and not {'Some', 'None'} & labels, items
        items = complete('package main; enum Box<T> { Value(T), Empty } func main() { Box<int>.Va| }')
        assert set(items) == {'Value'}, items
        items = complete('package main; interface Reader { Read() int; } func use(r Reader) { r.| } func main() {}')
        assert 'Read' in items, items
        items = complete('package main; func main() { | callback := func() {}; }')
        assert 'callback' not in items, items
        items = complete('package main; import "example.com/lsp/lib"; func main() { li| }')
        assert 'lib' in items, items
        items = complete('package main; enum Color { Red, Green } func (c Color) Read() int { return 1; } func main() { Color.| }')
        assert {'Red', 'Green'} <= items.keys() and 'Color' not in items, items
        # Syntax, scanner, binding, and semantic failures must not hide valid symbols.
        items = complete('package main; struct Person { Name string; } func main() { person := Person{}; broken := ; person.Na| }')
        assert 'Name' in items and any('syntax' in x['message'] for x in last_diagnostics), (items, last_diagnostics)
        items = complete('package main; func bad() { broken := ; } func main() { local := 1; lo| }')
        assert 'local' in items, items
        items = complete('package main; func main() { local := 1; @; lo| }')
        assert 'local' in items and any('scanner' in x['message'] for x in last_diagnostics), (items, last_diagnostics)
        items = complete('package main; func main() { local := 1; lo|')
        assert 'local' in items, items
        items = complete('package main; import "missing"; func main() { local := 1; lo| }')
        assert 'local' in items and any('could not resolve import: missing' in d['message'] for d in last_diagnostics), (items, last_diagnostics)
        items = complete('package main; func main() { bad: string = 7; local := 1; lo| }')
        assert 'local' in items and any('expected string, got int' in x['message'] for x in last_diagnostics), (items, last_diagnostics)
        # Malformed sibling files retain valid declarations and imports in this file.
        sibling.write_text('package main; func broken() { bad := ; } func helper() int { return 2; }')
        items = complete('package main; import "example.com/lsp/lib"; func main() { lib.Ma| }')
        assert 'Make' in items, items
        items = complete('package main; func main() { hel| }')
        assert 'helper' in items, items
        sibling.write_text('package main; func helper() int { return 2; }')
        # Semantic diagnostics survive unrelated syntax errors and point at calls.
        complete('package main;\nfunc take(x int) {}\nfunc main() {\n    broken := ;\n    take();\n    |\n}')
        arity = [d for d in last_diagnostics if 'Expected 1 args, but got 0.' in d['message']]
        assert arity and arity[0]['range']['start'] == {'line':4,'character':4}, last_diagnostics
        complete('package main;\nimport "example.com/lsp/lib";\nfunc main() {\n    lib.Make(42); |\n}')
        arity = [d for d in last_diagnostics if 'Expected 0 args, but got 1.' in d['message']]
        assert arity and arity[0]['range']['start']['line'] == 3, last_diagnostics
        # Package-name completion supplies an import edit, including declared names
        # that differ from the directory basename. Existing bindings suppress it.
        items = complete('package main; func main() { li| }')
        assert items['lib']['detail'] == 'example.com/lsp/lib', items
        edits = items['lib']['additionalTextEdits']
        assert edits == [{'range':{'start':{'line':0,'character':13},'end':{'line':0,'character':13}},
                          'newText':'\nimport "example.com/lsp/lib";'}], edits
        items = complete('package main; func main() { pre| }')
        assert items['pretty']['detail'] == 'formatting', items
        assert items['pretty']['additionalTextEdits'][0]['newText'] == '\nimport "formatting";', items
        items = complete('package main; import "example.com/lsp/lib"; func main() { li| }')
        assert 'additionalTextEdits' not in items['lib'], items
        items = complete('package main; func main() { lib := 1; li| }')
        assert 'additionalTextEdits' not in items['lib'], items
        items = complete('package main; import "example.com/lsp/lib" as renamed; func main() { li| }')
        assert 'lib' not in items, items
        # A syntax error in a test file must retain imports and semantic completion.
        testfile = root/'other_test.oc'
        testfile.write_text('package main; func brokenTest() { bad := ; }')
        items = complete('package main; func main() { li| }')
        assert items['lib']['detail'] == 'example.com/lsp/lib', items
        testfile.unlink()
        # Declared dependencies are discovered from local replacements without downloads.
        replacement = root/'replacement'; replacement.mkdir()
        (replacement/'ocean.mod').write_text('module example.com/dependency\nocean 0.1\n')
        (replacement/'dep.oc').write_text('package dependency; func Export() {}')
        manifest = root/'ocean.mod'
        original = manifest.read_text()
        manifest.write_text(original + 'replace example.com/dependency => ./replacement\n')
        items = complete('package main; func main() { dep| }')
        assert items['dependency']['detail'] == 'example.com/dependency', items
        manifest.write_text(original)
        # Apply both the chosen completion and the import, then complete its exports.
        buffer = 'package main; func main() { lib.| }'
        insert = edits[0]['range']['start']['character']
        buffer = buffer[:insert] + edits[0]['newText'] + buffer[insert:]
        items = complete(buffer)
        assert {'Make','Thing'} <= items.keys(), items
        send('shutdown',{},99);assert recv()['result'] is None;send('exit');assert server.wait(timeout=10)==0
        assert not server.stderr.read()
    finally:
        if server.poll() is None:server.kill();server.wait()
print('semantic LSP checks passed')
