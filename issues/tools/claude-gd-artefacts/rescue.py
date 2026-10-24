# rescue.py <mask> <fail_threshold>: where the default methods fail, how often does a gd variant fix it?
# "others" = the better of opposed and segments per image (the user would try both first)
import json,glob,os,sys,numpy as np
mk=sys.argv[1]; T=float(sys.argv[2])
S={}
for run in ['full1','full2']:
    for f in glob.glob(run+'/*/scores.json'):
        n=os.path.basename(os.path.dirname(f))
        S.setdefault(n,{}).update(json.load(open(f))['methods'])
G=['gd','gd-wb4x2','gd-wb4x6','gd-wb4x7','gd-wb4x8','gd-wb4x9']
src=lambda n: n.split('_')[0]
rows=[]
for n,v in S.items():
    if mk not in v['opposed'] or any(g not in v or mk not in v[g] for g in G): continue
    o=min(v['opposed'][mk]['dE_mean'],v['segments'][mk]['dE_mean'])
    rows.append((n,o,{g:v[g][mk]['dE_mean'] for g in G}))
fail=[r for r in rows if r[1]>T]
print('mask %s: %d images, %d where the better of opposed/segments has dE > %g'%(mk,len(rows),len(fail),T))
def fixed(o,g): return g<=o-2 and g<=0.8*o   # clearly better: >= 2 dE and >= 20%
def broke(o,g): return g>=o+2 and g>=1.25*o
print('%-9s %8s %8s %8s %8s %10s'%('variant','fixed','worse','mean','others','broke(ok)'))
for g in G:
    fx=sum(fixed(o,d[g]) for _,o,d in fail); wo=sum(broke(o,d[g]) for _,o,d in fail)
    ok=[r for r in rows if r[1]<=T]; bk=sum(broke(o,d[g]) for _,o,d in ok)
    print('%-9s %8d %8d %8.2f %8.2f %6d/%d'%(g,fx,wo,np.mean([d[g] for _,_,d in fail]),np.mean([o for _,o,_ in fail]),bk,len(ok)))
anyfix=[r for r in fail if any(fixed(r[1],r[2][g]) for g in G)]
best={}
for n,o,d in anyfix:
    b=min(G,key=lambda g:d[g]); best[b]=best.get(b,0)+1
print('fixed by at least one variant: %d of %d; best variant there: %s'%(len(anyfix),len(fail),best))
print('by source (failing / fixed by wb4x7 / by wb4x9 / by any):')
for s in sorted(set(src(n) for n,_,_ in rows)):
    f=[r for r in fail if src(r[0])==s]
    print('  %-6s %3d / %3d / %3d / %3d   (of %d)'%(s,len(f),sum(fixed(o,d['gd-wb4x7']) for _,o,d in f),sum(fixed(o,d['gd-wb4x9']) for _,o,d in f),sum(any(fixed(o,d[g]) for g in G) for _,o,d in f),sum(src(r[0])==s for r in rows)))
if len(sys.argv)>3:
    for n,o,d in sorted(fail,key=lambda r:-r[1]):
        print('  %-28s %5.1f  '%(n[:28],o)+' '.join('%5.1f%s'%(d[g],'*' if fixed(o,d[g]) else ('!' if broke(o,d[g]) else ' ')) for g in G))
