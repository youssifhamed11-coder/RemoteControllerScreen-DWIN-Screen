import math, json, unicodedata, os
FONT=os.environ.get('AR_FONT','DejaVuSans-Bold.ttf')   # path to DejaVu Sans Bold
from PIL import Image, ImageDraw, ImageFont
from shape import shape, visual, IDX
glyphs=json.load(open('glyphs.json'))
def render_lib(cell, fsize):
    BW=math.ceil(cell/8); SZ=BW*cell
    font=ImageFont.truetype(FONT,fsize)
    base=int(cell*0.78)
    def rend(cp):
        img=Image.new('1',(cell*3,cell*2),0); dr=ImageDraw.Draw(img)
        a,_=font.getmetrics(); dr.text((cell,base+cell//4-a),chr(cp),font=font,fill=1); return img,img.getbbox()
    timg,_=rend(0x0640); trows=[y-cell//4 for y in range(cell*2) if any(timg.getpixel((x,y)) for x in range(cell*3))]
    def joins(cp):
        dec=unicodedata.decomposition(chr(cp))
        return 'L' if dec.startswith('<initial>') else 'LR' if dec.startswith('<medial>') or cp==0x640 else 'R' if dec.startswith('<final>') else ''
    cells=[]
    for cp in glyphs:
        c=Image.new('1',(cell,cell),0); img,bb=rend(cp)
        if bb:
            g=img.crop(bb); gw,gh=g.size
            if gw>cell: g=g.resize((cell,gh)); gw=cell
            j=joins(cp); x0=cell-gw-1 if j=='L' else 1 if j=='R' else (cell-gw)//2
            c.paste(g,(x0,bb[1]-cell//4)); dr=ImageDraw.Draw(c)
            for y in trows:
                if 'L' in j: dr.line([(0,y),(x0+2,y)],fill=1)
                if 'R' in j: dr.line([(x0+gw-3,y),(cell-1,y)],fill=1)
        b=bytearray(SZ)
        for y in range(cell):
            for x in range(cell):
                if c.getpixel((x,y)): b[y*BW+x//8]|=0x80>>(x%8)
        cells.append(bytes(b))
    return cells,SZ
def pack(cells,SZ,offset_of,total):
    out=bytearray(total)
    for i,c in enumerate(cells): o=offset_of(i)*SZ; out[o:o+SZ]=c
    return bytes(out)
# code schemes: index -> (H,L), slot offset
gb2312=lambda i:(0xA1+i//94,0xA1+i%94)
gbk63 =lambda i:(0x81+i//63,0x40+i%63)
cells40,SZ40=render_lib(40,34); cells32,SZ32=render_lib(32,27)
O='out'   # 28_AR.HZK (GB2312 40x40) = the font used in production; the others are test variants
import os; os.makedirs(O,exist_ok=True)
open(O+'/28_AR.HZK','wb').write(pack(cells40,SZ40,lambda i:i,188*SZ40))                       # GB2312 40
open(O+'/29_ARK192.HZK','wb').write(pack(cells40,SZ40,lambda i:(i//63)*192+i%63,3*192*SZ40))  # GBK stride 192
open(O+'/30_ARK190.HZK','wb').write(pack(cells40,SZ40,lambda i:(i//63)*190+i%63,3*190*SZ40))  # GBK stride 190
open(O+'/31_AR32.HZK','wb').write(pack(cells32,SZ32,lambda i:i,188*SZ32))                     # GB2312 32
print('written to', O)
