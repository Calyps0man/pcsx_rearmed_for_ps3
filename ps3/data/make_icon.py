"""App icon (ICON0.PNG, 320x176): large "PS1" lettering on a grey panel, with the app name.
usage: python3 make_icon.py [out.png]"""
import sys
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter
S=4; W,H=320*S,176*S
B='/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf'
def bg(c1,c2):
    im=Image.new('RGB',(W,H)); px=im.load()
    for y in range(H):
        t=y/H
        c=tuple(int(c1[i]*(1-t)+c2[i]*t) for i in range(3))
        for x in range(W): px[x,y]=c
    return im.convert('RGBA')
def fin(im,name): im.convert('RGB').resize((320,176),Image.LANCZOS).save(name)

# B: big typographic "PS1" badge, plain font, grey console-like palette
im=bg((205,205,210),(150,150,158)); d=ImageDraw.Draw(im)
d.rounded_rectangle([14*S,14*S,306*S,162*S],radius=18*S,outline=(110,110,118),width=3*S)
fb=ImageFont.truetype(B,92*S)
t="PS1"; w=d.textlength(t,font=fb)
d.text(((W-w)/2+3*S,10*S+3*S),t,font=fb,fill=(120,120,128))
d.text(((W-w)/2,10*S),t,font=fb,fill=(40,42,50))
fs=ImageFont.truetype(B,20*S); t2="PCSX-ReARMed"; w2=d.textlength(t2,font=fs)
d.text(((W-w2)/2,124*S),t2,font=fs,fill=(40,42,50))
fin(im, sys.argv[1] if len(sys.argv) > 1 else 'ICON0.PNG')

