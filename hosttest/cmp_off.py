import sys
from PIL import Image
a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB')
best=None
for dy in range(-3,4):
    w=min(a.size[0],b.size[0]); h=min(a.size[1],b.size[1])-3
    pa=a.load(); pb=b.load(); bad=0; n=0
    for y in range(3,h):
        if not (0<=y+dy<b.size[1]): continue
        for x in range(w):
            p=pa[x,y]; q=pb[x,y+dy]; n+=1
            if max(abs(p[i]//8-q[i]//8) for i in range(3))>1: bad+=1
    r=100.0*bad/max(n,1)
    if best is None or r<best[0]: best=(r,dy)
print(f"{best[0]:.2f}% px differ (row offset {best[1]})")
