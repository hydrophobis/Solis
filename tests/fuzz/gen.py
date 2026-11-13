"""Random programs over ints, floats, bools, an int array and one struct.

Usage: gen.py <seed>. Used by tools/fuzz.sh.
"""
import random,sys
seed=int(sys.argv[1]); R=random.Random(seed)
ivars=['a','b','c','d']; fvars=['x','y']; bvars=['p','q']; 
def iexpr(dep=0,vs=ivars):
    r=R.random()
    if dep>3 or r<0.25: return R.choice(vs) if R.random()<0.6 else str(R.randint(0,40))
    if r<0.55:
        return "(%s %s %s)"%(iexpr(dep+1,vs),R.choice(['+','-','*']),iexpr(dep+1,vs))
    if r<0.62: return "(%s / %d)"%(iexpr(dep+1,vs),R.randint(1,7))
    if r<0.68: return "(%s %% %d)"%(iexpr(dep+1,vs),R.randint(1,9))
    if r<0.76: return "arr[((%s) %% 5 + 5) %% 5]"%iexpr(dep+1,vs)
    if r<0.82: return "len(arr)"
    if r<0.88: return "f%d(%s, %s)"%(R.randint(0,1),iexpr(dep+1,vs),iexpr(dep+1,vs))
    if r<0.93: return "s.n"
    return "(0 - %s)"%iexpr(dep+1,vs)
def fexpr(dep=0):
    r=R.random()
    if dep>2 or r<0.3: return R.choice(fvars) if R.random()<0.6 else "%d.%d"%(R.randint(0,9),R.randint(0,9))
    return "(%s %s %s)"%(fexpr(dep+1),R.choice(['+','-','*']),fexpr(dep+1))
def bexpr(dep=0):
    r=R.random()
    if dep>2 or r<0.2: return R.choice(bvars+['true','false'])
    if r<0.5: return "(%s %s %s)"%(iexpr(dep+1),R.choice(['<','<=','>','>=','==','!=']),iexpr(dep+1))
    if r<0.62: return "(%s %s %s)"%(fexpr(dep+1),R.choice(['<','<=','>','>=','==','!=']),fexpr(dep+1))
    if r<0.78: return "(%s && %s)"%(bexpr(dep+1),bexpr(dep+1))
    if r<0.9: return "(%s || %s)"%(bexpr(dep+1),bexpr(dep+1))
    return "!%s"%bexpr(dep+1)
def stmt(dep=0,inloop=False):
    r=R.random(); ind="    "*(dep+1)
    if r<0.22: return ind+"%s = %s;"%(R.choice(ivars),iexpr())
    if r<0.30: return ind+"%s += %s;"%(R.choice(ivars),iexpr())
    if r<0.34: return ind+"%s = %s;"%(R.choice(fvars),fexpr())
    if r<0.38: return ind+"%s = %s;"%(R.choice(bvars),bexpr())
    if r<0.44: return ind+"arr[((%s) %% 5 + 5) %% 5] = %s;"%(iexpr(),iexpr())
    if r<0.48: return ind+"s.n = %s;"%iexpr()
    if r<0.52: return ind+"s.n += %s;"%iexpr()
    if r<0.56: return ind+'str = str + "${%s}," ;'%iexpr()
    if r<0.60: return ind+"push(arr, %s);"%iexpr()
    if r<0.64: return ind+"let t%d = s;\n%st%d.n = %s;\n%sprint(\"${t%d.n} ${s.n}\");"%(dep*10+R.randint(0,9),ind,dep*10+R.randint(0,9),iexpr(),ind,dep*10+R.randint(0,9)) if False else ind+"print(\"${%s} ${%s}\");"%(iexpr(),fexpr())
    if r<0.78 and dep<2:
        body="\n".join(stmt(dep+1,inloop) for _ in range(R.randint(1,3)))
        s=ind+"if %s {\n%s\n%s}"%(bexpr(),body,ind)
        if R.random()<0.5:
            s+=" else {\n%s\n%s}"%("\n".join(stmt(dep+1,inloop) for _ in range(R.randint(1,2))),ind)
        return s
    if r<0.90 and dep<2:
        v="i%d"%dep
        body="\n".join(stmt(dep+1,True) for _ in range(R.randint(1,3)))
        return ind+"var %s = 0;\n%swhile %s < %d {\n%s\n%s    %s += 1;\n%s}"%(v,ind,v,R.randint(1,6),body,ind,v,ind)
    return ind+"print(\"${%s}\");"%iexpr()
prog='''struct S { n: int; }
func f0(u: int, v: int): int { if u > v { return u - v; } return v * 2 + 1; }
func f1(u: int, v: int): int { var r = 0; var k = 0; while k < 3 { r += u * k - v; k += 1; } return r; }
func main() {
    var a = %d; var b = %d; var c = %d; var d = %d;
    var x = 1.5; var y = 0.25;
    var p = true; var q = false;
    var arr = [1, 2, 3, 4, 5];
    var s = S { n: 7 };
    var str = "";
%s
    print("${a} ${b} ${c} ${d} ${x} ${y} ${p} ${q} ${arr} ${s.n} ${str}");
}
'''%(R.randint(0,9),R.randint(0,9),R.randint(0,9),R.randint(0,9),"\n".join(stmt() for _ in range(R.randint(4,10))))
print(prog)
