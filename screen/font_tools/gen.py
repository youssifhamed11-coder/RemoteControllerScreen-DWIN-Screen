import math, unicodedata, json
from PIL import Image, ImageDraw, ImageFont
import sys, os
# usage: python gen.py <path to 0_DWIN_ASC.HZK from the DGUS folder>
SRC=sys.argv[1] if len(sys.argv)>1 else '0_DWIN_ASC.HZK'
FONT=os.environ.get('AR_FONT','DejaVuSans-Bold.ttf')
d=open(SRC,'rb').read()
off=lambda n: sum(math.ceil(k/8)*2*k*128 for k in range(4,n))
# ---- 1) ASCII lib (0# format) sizes 4..32 -> 24_ASC.HZK
asc=d[:off(33)]
open('24_ASC.HZK','wb').write(asc)
# ---- 2) Arabic glyph lib, GB2312 layout, 40x40 cells
W=H=40; BW=5; SZ=BW*H
glyphs=[]                    # list of codepoints in index order
glyphs+= list(range(0xFE70,0xFEFD))            # presentation forms B (141)
extra=[0x060C,0x061B,0x061F,0x0640]+list(range(0x0660,0x066A))
glyphs+=extra
font=ImageFont.truetype(FONT,34)
BASE=31   # baseline row (ASCII 'B' bottom row is 31)
# stroke thickness / position of the joining line from tatweel
def render(cp):
    img=Image.new('1',(120,60),0); dr=ImageDraw.Draw(img)
    ch=chr(cp)
    asc_,desc_=font.getmetrics()
    dr.text((40,BASE+10-asc_),ch,font=font,fill=1)
    bb=img.getbbox()
    return img,bb
timg,tbb=render(0x0640)
# rows of tatweel ink
trows=[y for y in range(60) if any(timg.getpixel((x,y)) for x in range(120))]
def joins(cp):
    name=unicodedata.name(chr(cp),'')
    dec=unicodedata.decomposition(chr(cp))
    if dec.startswith('<initial>'): return 'L'      # connects to the following letter = on its LEFT side
    if dec.startswith('<medial>'): return 'LR'
    if dec.startswith('<final>'): return 'R'
    if cp==0x0640: return 'LR'
    return ''
cells=[]
for cp in glyphs:
    cell=Image.new('1',(W,H),0)
    try:
        img,bb=render(cp)
    except Exception: bb=None
    if bb:
        g=img.crop(bb); gw,gh=g.size
        if gw>W: g=g.resize((W,gh)); gw=W
        j=joins(cp)
        if j=='L': x0=W-gw-2            # hug right side, line runs to the left edge
        elif j=='R': x0=2
        else: x0=(W-gw)//2
        y0=bb[1]-10
        cell.paste(g,(x0,y0))
        dr=ImageDraw.Draw(cell)
        for y in trows:
            yy=y-10
            if 'L' in j: dr.line([(0,yy),(x0+2,yy)],fill=1)
            if 'R' in j: dr.line([(x0+gw-3,yy),(W-1,yy)],fill=1)
    cells.append(cell)
out=bytearray(SZ*188)
for i,c in enumerate(cells):
    for y in range(H):
        for bx in range(BW):
            b=0
            for k in range(8):
                x=bx*8+k
                if x<W and c.getpixel((x,y)): b|=0x80>>k
            out[i*SZ+y*BW+bx]=b
open('28_AR.HZK','wb').write(out)
json.dump(glyphs,open('glyphs.json','w'))
print('asc',len(asc),'ar',len(out),'glyphs',len(glyphs),'tatweel rows',[y-10 for y in trows])
