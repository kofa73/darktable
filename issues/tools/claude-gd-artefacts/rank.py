# rank.py <run> <method> <mask> [N]: per image, opposed vs method on one mask
import json,glob,os,sys,numpy as np
run,meth,mk=sys.argv[1:4]; N=int(sys.argv[4]) if len(sys.argv)>4 else 25
S={}
for f in glob.glob(run+'/*/scores.json'):
    S[os.path.basename(os.path.dirname(f))]=json.load(open(f))['methods']
P=set(os.path.basename(os.path.dirname(f)) for f in glob.glob('panel3/*/scores.json'))
rows=[]
for n,v in S.items():
    if mk not in v['opposed']: continue
    o=v['opposed'][mk]; g=v[meth][mk]
    rows.append((g['dE_mean']-o['dE_mean'],n,o,g))
rows.sort()
def pr(r):
    d,n,o,g=r
    print('%-26s %7d %5.1f %5.1f  %5.1f %5.1f  %5.1f %5.1f  %+.2f %+.2f  %5.1f %5.1f %s'%(n[:26],o['n'],o['dE_mean'],g['dE_mean'],o['dE_p50'],g['dE_p50'],1000*o['duv_mean'],1000*g['duv_mean'],o['dlogY'],g['dlogY'],o['dC_mean'],g['dC_mean'],'P' if n in P else ''))
print('name                             n    dE o/g     p50 o/g    duv o/g    dlogY o/g     dC o/g')
print('--- worst for',meth); [pr(r) for r in rows[::-1][:N]]
print('--- best for',meth); [pr(r) for r in rows[:N//2]]
d=np.array([r[0] for r in rows]); print('median diff %.2f, >0.5: %d, <-0.5: %d'%(np.median(d),(d>0.5).sum(),(d<-0.5).sum()))
