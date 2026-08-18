import sys, zlib, struct
w,h=320,240
d=open(sys.argv[1],'rb').read()
px=struct.unpack('<%dH'%(w*h), d[:w*h*2])
rows=bytearray()
for y in range(h):
    rows.append(0)
    for x in range(w):
        v=px[y*w+x]; r=(v>>10)&31; g=(v>>5)&31; b=v&31
        rows+=bytes((r*255//31, g*255//31, b*255//31))
def ch(t,dd): return struct.pack('>I',len(dd))+t+dd+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(bytes(rows),9))+ch(b'IEND',b''))
print(sys.argv[2], len(set(px)),"colours")
