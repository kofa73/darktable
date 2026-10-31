# wbcheck.py: frames of a pair with different as-shot white balance or ISO would bias the score
import json, rawpy, numpy as np, subprocess
rows=[]
for e in json.load(open('plan.json')):
    w=[]
    for f in (e['clipped'], e['ref']):
        with rawpy.imread(f) as r:
            c=np.array(r.camera_whitebalance[:3],float); w.append(c/c[1])
    iso=subprocess.run(['exiftool','-q','-T','-ISO','-ExposureTime',e['clipped'],e['ref']],capture_output=True,text=True).stdout.split('\n')
    s=json.load(open('/tmp/hl-rules-study/runs/xtrans/'+e['name']+'/scores.json'))
    d=np.abs(np.log2(w[0]/w[1]))
    pa=json.load(open('/tmp/hl-rules-study/runs/xtrans/'+e['name']+'/pair_align-xt.json'))
    print('%-34s wb c %.2f/%.2f t %.2f/%.2f dlog %.3f %.3f  %s | %s  shift %.1f %.1f resp %.2f motion %.0f%% changed %.1f%%' % (e['name'],w[0][0],w[0][2],w[1][0],w[1][2],d[0],d[2],iso[0].replace('\t',' '),iso[1].replace('\t',' '),s['pair_shift'][0],s['pair_shift'][1],s['pair_shift'][2],100*(pa.get('motion') or 0),100*pa.get('changed_fraction',0)))
