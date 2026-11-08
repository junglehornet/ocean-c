"""Recursive value equality, unconstrained generics, boxed values, and string order."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=25)


with tempfile.TemporaryDirectory(prefix="ocean equality ") as directory:
    root = Path(directory)
    source, ir, executable = root / "main.oc", root / "main.ll", root / "native"
    checks = 0

    def case(body):
        global checks
        source.write_text("package main; " + body)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stderr
        for optimization in ("-O0", "-O2"):
            built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable), "-lffi"])
            assert built.returncode == 0, built.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (body, optimization, result)
            checks += 1

    case('struct S { flag bool; n int; } func main() int { a:=S{true,7}; b:=S{true,7}; if(a!=b) return 1; b.n=8; if(a==b || !(a!=b)) return 2; return 0; }')
    case('struct Empty {} func main() int { if(Empty{}!=Empty{}) return 1; return 0; }')
    case('struct Inner { s string; } struct Outer { value Inner; xs int[]; } func main() int { a:=Outer{Inner{"hi"},{1,2}}; b:=Outer{Inner{"hi"},{1,2}}; if(a!=b) return 1; b.value.s="bye"; if(a==b) return 2; return 0; }')
    case('func main() int { a:=int[3]{1,2,3}; b:=int[3]{1,2,3}; if(a!=b) return 1; b[2]=4; if(a==b) return 2; if(a==int[2]{1,2}) return 3; return 0; }')
    case('func main() int { a: int[] = {1,2}; b: int[] = {1,2}; b.append(3); b.pop(); if(a!=b) return 1; b[1]=4; if(a==b) return 2; return 0; }')
    case('func main() int { a:=int[2]{1,2}; b: int[] = {1,2}; if(a!=b || b!=a) return 1; if(a==int[1]{1}) return 2; return 0; }')
    case('func main() int { n:=2; a: int[n] = {1,2}; b: int[] = {1,2}; if(a!=b) return 1; return 0; }')
    case('func main() int { a: int[][] = {{1,2},{3}}; b: int[][] = {{1,2},{3}}; if(a!=b) return 1; b[1][0]=4; if(a==b) return 2; return 0; }')
    case('struct S { x int; } func main() int { a:=S[2]{{1},{2}}; b:=S[2]{{1},{2}}; if(a!=b) return 1; b[1].x=3; if(a==b) return 2; return 0; }')
    case('func same<T>(a,b T) bool { return a==b; } func different<T>(a,b T) bool { return a!=b; } struct S { s string; } func main() int { if(!same<S>(S{"hi"},S{"hi"}) || !different<S>(S{"hi"},S{"bye"})) return 1; if(!same<int[]>(int[]{1,2},int[]{1,2})) return 2; return 0; }')
    case('struct S { s string; } func indexOf<T>(xs array<T>, value T) int { for(i:=0;i<len(xs);i++) if(xs[i]==value) return i; return -1; } func main() int { xs:=S[2]{{"a"},{"b"}}; if(indexOf<S>(xs,S{"b"})!=1 || indexOf<S>(xs,S{"z"})!=-1) return 1; return 0; }')
    case('struct Node<T> { value T; next *Node<T>; } struct List<T> { head *Node<T>; } func (l List<T>) indexOf(value T) int { node:=l.head; for(i:=0;node!=null;i++) { if(node.value==value) return i; node=node.next; } return -1; } struct Person { name string; age int; } func main() int { b:=Node<Person>{Person{"b",2},null}; a:=Node<Person>{Person{"a",1},&b}; l:=List<Person>{&a}; if(l.indexOf(Person{"b",2})!=1 || l.indexOf(Person{"b",3})!=-1) return 1; return 0; }')
    case('struct S { n int; } func same(a,b any) bool { return a==b; } func main() int { if(!same(S{2},S{2}) || same(S{2},S{3}) || same(S{2},2)) return 1; if(!same(int[]{1,2},int[]{1,2}) || same(int[]{1},int[]{1,2})) return 2; if(!same(2,2->i8) || !same(2,2)) return 3; return 0; }')
    case('struct S { v any; } func main() int { a:=S{int[]{1,2}}; b:=S{int[]{1,2}}; if(a!=b) return 1; b.v=int[]{3}; if(a==b) return 2; return 0; }')
    case('func main() int { a: mixed any[] = {1,"a",int[]{2,3}}; b: mixed any[] = {1,"a",int[]{2,3}}; if(a!=b) return 1; b[1]="b"; if(a==b) return 2; return 0; }')
    case('enum E { A, V(int), S { name string; } } func main() int { a:=E.V(2); b:=E.V(2); c:=E.V(3); if(a!=b || a==c || a==E.A) return 1; if(a!=E.V) return 2; if(E.S{name:"x"}!=E.S{name:"x"}) return 3; return 0; }')
    case('enum Tree { Leaf(int), Branch(Tree[]) } func same<T>(a,b T) bool { return a==b; } func main() int { a:=Tree.Branch(Tree[]{Tree.Leaf(1),Tree.Leaf(2)}); b:=Tree.Branch(Tree[]{Tree.Leaf(1),Tree.Leaf(2)}); c:=Tree.Branch(Tree[]{Tree.Leaf(1),Tree.Leaf(3)}); if(!same<Tree>(a,b) || same<Tree>(a,c)) return 1; return 0; }')
    case('enum E<T> { V(T) } func main() int { a:=E<int[]>.V(int[]{1,2}); b:=E<int[]>.V(int[]{1,2}); c:=E<int[]>.V(int[]{1,3}); if(a!=b || a==c) return 1; return 0; }')
    case('struct S { x int; p *int; } func main() int { n:=2; m:=2; a:=S{1,&n}; b:=S{1,&n}; c:=S{1,&m}; if(a!=b || a==c || &n==&m) return 1; return 0; }')
    case('func main() int { a:=buffer<int>[2]{1,2}; b:=buffer<int>[2]{1,2}; if(a!=b) return 1; b[1]=3; if(a==b) return 2; p:buffer<int> = a; q:buffer<int> = p; r:buffer<int> = b; if(p!=q || p==r) return 3; return 0; }')
    case('func name() int {return 1;} func main() int { a:=name; b:=name; if(a!=b) return 1; f:=func() int {return 1;}; g:=f; if(f!=g) return 2; return 0; }')
    case('func make(n int) func() int { return func() int {return n;}; } func same<T>(a,b T) bool {return a==b;} func main() int { a:=make(2); b:=make(2); if(same<func() int>(a,b) || !same<func() int>(a,a)) return 1; return 0; }')
    case('func nan() float { return 0.0/0.0; } struct S { f float; } func main() int { a:=float[1]{nan()}; if(a==a || !(a!=a)) return 1; x:=S{nan()}; if(x==x) return 2; p:=float[1]{0.0}; n:=float[1]{-0.0}; if(p!=n) return 3; return 0; }')
    case('func mutate(a *int[]) int[] { (*a)[0]=2; return *a; } func main() int { a:int[]={1}; if(a==mutate(&a)) return 1; return 0; }')
    case('struct S { s string; } func mutate(a *S) S { a.s="new"; return *a; } func main() int { a:=S{"old"}; if(a==mutate(&a)) return 1; return 0; }')
    case('type ID int; struct S { x ID; } func same<T>(a,b T) bool {return a==b;} func main() int { if(!same<S>(S{2->ID},S{2->ID})) return 1; return 0; }')
    case('interface Text { string } func less<T string>(a,b T) bool {return a<b;} func order(a,b Text) bool {return a<=b;} func main() int { if(!less<string>("a","b") || !order("a","ab") || order("z","a")) return 1; return 0; }')
    case('func main() int { if(!("a"<"b" && "a"<="a" && "b">"a" && "b">="b")) return 1; if("a">"aa" || "aa"<="a" || ""!="" || !(""<"a")) return 2; if(!("a\\0b"<"a\\0c")) return 3; if(!("é">"z")) return 4; return 0; }')

    case('type Name string; func less<T ~string>(a,b T) bool {return a<b;} func main() int { if(!less<Name>("a"->Name,"b"->Name)) return 1; return 0; }')
    case('func equal(a,b any) bool {return a==b;} func main() int { x: f32=2.5; y: float=2.5; if(!equal(x,y) || equal(x,3)) return 1; return 0; }')
    case('type F=func() int; struct S { f F; } func make(n int) F {return func() int {return n;};} func main() int { f:=make(1); a:=S{f}; b:=a; c:=S{make(1)}; if(a!=b || a==c) return 1; return 0; }')
    case('func main() int { a:=int[0]{}; b:=make(int[]); if(a!=b) return 1; xs: int[][] = {{}}; ys: int[][] = {{}}; if(xs!=ys) return 2; return 0; }')

    case('type Flag bool; struct S { value Flag; } func equal<T>(a,b T) bool {return a==b;} func main() int { t:=true->Flag; f:=false->Flag; if(!equal<Flag>(t,t) || equal<Flag>(t,f) || S{t}!=S{t}) return 1; x:any=t; y:any=t; if(x!=y) return 2; return 0; }')
    case('type Name string; interface Text { ~string } func less(a,b Text) bool {return a<b;} func main() int { if(!less("a"->Name,"b"->Name)) return 1; return 0; }')

    # Unused generic definitions must also accept equality without constraints.
    source.write_text('package main; func equal<T>(a,b T) bool { return a==b; } func main() {}')
    result = run([compiler,'check',str(source)])
    assert result.returncode == 0, result.stderr

    for body in (
        'struct A { x int; } struct B { x int; } func main() { a:=A{1}; b:=B{1}; a==b; }',
        'func main() { a:=int[1]{1}; b:=bool[1]{true}; a==b; }',
        'func less<T>(a,b T) bool {return a<b;} func main() {}',
    ):
        source.write_text('package main; '+body)
        result = run([compiler,'check',str(source)])
        assert result.returncode != 0, result

print(f"{checks} equality and string ordering checks passed at -O0 and -O2")
