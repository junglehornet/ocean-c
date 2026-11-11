"""Semantic LSP hover, navigation, references, rename and contextual completion."""
from pathlib import Path
import json, os, select, subprocess, sys, tempfile
compiler=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='ocean semantic lsp ') as d:
    root=Path(d);source=root/'main.oc';sibling=root/'other.oc'
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
    server=subprocess.Popen([compiler,'check','--lsp'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
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
            global version
            version += 1
            before, after = buffer.split('|')
            edited = before + after
            send('textDocument/didChange', {'textDocument': {'uri': source.as_uri(), 'version': version},
                                          'contentChanges': [{'text': edited}]})
            recv()
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
        send('shutdown',{},99);assert recv()['result'] is None;send('exit');assert server.wait(timeout=10)==0
        assert not server.stderr.read()
    finally:
        if server.poll() is None:server.kill();server.wait()
print('semantic LSP checks passed')
