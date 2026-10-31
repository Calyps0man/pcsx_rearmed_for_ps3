import sys, struct
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
A = struct.unpack('<%dH' % (len(a) // 2), a); B = struct.unpack('<%dH' % (len(b) // 2), b)
col = sum(1 for x, y in zip(A, B) if (x ^ y) & 0x7fff)
b15 = sum(1 for x, y in zip(A, B) if (x ^ y) == 0x8000)
print("vram: %d colour, %d bit-15-only differences" % (col, b15))
