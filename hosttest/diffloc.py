import sys, struct
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
A = struct.unpack('<524288H', a); B = struct.unpack('<524288H', b)
n=0
rows={}
for i,(x,y) in enumerate(zip(A,B)):
    if (x^y)&0x7fff:
        r=i//1024; rows.setdefault(r,[]).append((i%1024,x,y))
for r in sorted(rows)[:int(sys.argv[3]) if len(sys.argv)>3 else 12]:
    l=rows[r]; print(r, len(l), 'x %d..%d'%(l[0][0],l[-1][0]), ' '.join('%d:%04x/%04x'%t for t in l[:6]))
print(len(rows),'rows')
