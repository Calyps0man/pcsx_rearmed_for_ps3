import sys
from PIL import Image, ImageChops
a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB')
if a.size!=b.size: print('size', a.size, b.size); sys.exit()
pa=a.load(); pb=b.load(); w,h=a.size; bad=0
for y in range(h):
    for x in range(w):
        p=pa[x,y]; q=pb[x,y]
        if max(abs(p[i]//8-q[i]//8) for i in range(3))>1: bad+=1
print(f"{sys.argv[1]}: {100.0*bad/(w*h):.2f}% px differ")
if len(sys.argv)>3:
    c=Image.new('RGB',(w*2+4,h)); c.paste(a,(0,0)); c.paste(b,(w+4,0)); c.save(sys.argv[3])
