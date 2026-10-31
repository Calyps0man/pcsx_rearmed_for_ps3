import sys,struct
from PIL import Image
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
A=struct.unpack('<524288H',a); B=struct.unpack('<524288H',b)
col=0; b15=0
img=Image.new('RGB',(1024,512))
px=img.load()
for i in range(524288):
    x,y=i%1024,i//1024
    va,vb=A[i],B[i]
    d=any(abs(((va>>s)&31)-((vb>>s)&31))>1 for s in (0,5,10))
    if d: col+=1; px[x,y]=(255,0,0)
    elif (va^vb)&0x8000: b15+=1; px[x,y]=(0,255,255)
    else:
        px[x,y]=(((vb&31)<<3)>>2,(((vb>>5)&31)<<3)>>2,(((vb>>10)&31)<<3)>>2)
print('colour diffs',col,'bit15-only',b15)
if len(sys.argv)>3: img.save(sys.argv[3])
