import unicodedata, json
glyphs=json.load(open('glyphs.json')); IDX={cp:i for i,cp in enumerate(glyphs)}
# forms table base -> [iso, fin, ini, med]
forms={}
for cp in range(0xFE70,0xFEFD):
    dec=unicodedata.decomposition(chr(cp)).split()
    if len(dec)!=2: continue
    kind,base=dec[0],int(dec[1],16)
    k={'<isolated>':0,'<final>':1,'<initial>':2,'<medial>':3}.get(kind)
    if k is None: continue
    forms.setdefault(base,[0,0,0,0])[k]=cp
def is_ar(c): return 0x0621<=c<=0x064A
def dual(c): f=forms.get(c); return bool(f and f[2])
LAMALEF={0x0622:0xFEF5,0x0623:0xFEF7,0x0625:0xFEF9,0x0627:0xFEFB}
def shape(text):
    cps=[ord(ch) for ch in text if not (0x064B<=ord(ch)<=0x0652)]
    out=[]; i=0
    def joins_prev(k):   # previous letter can connect forward to us
        return k>=0 and is_ar(cps[k]) and dual(cps[k])
    while i<len(cps):
        c=cps[i]
        if c==0x0644 and i+1<len(cps) and cps[i+1] in LAMALEF:
            out.append(LAMALEF[cps[i+1]]+(1 if joins_prev(i-1) else 0)); i+=2; continue
        if is_ar(c) and c in forms:
            p=joins_prev(i-1)
            n=dual(c) and i+1<len(cps) and is_ar(cps[i+1]) and cps[i+1] in forms
            f=forms[c]
            g = f[3] if (p and n) else f[1] if p else f[2] if n else f[0]
            out.append(g or f[0])
        else: out.append(c)
        i+=1
    return out
def visual(seq):   # RTL paragraph: reverse, keep LTR runs (latin/digits) in order; spaces are LTR only inside an LTR run
    isl=lambda c: c<0x80 and c!=0x20
    dirs=[]
    for k,c in enumerate(seq):
        if c!=0x20: dirs.append(isl(c)); continue
        a=next((isl(x) for x in reversed(seq[:k]) if x!=0x20),False)
        b=next((isl(x) for x in seq[k+1:] if x!=0x20),False)
        dirs.append(a and b)
    runs=[]
    for c,dr in zip(seq,dirs):
        if runs and runs[-1][0]==dr: runs[-1][1].append(c)
        else: runs.append((dr,[c]))
    out=[]
    for ltr,r in reversed(runs): out += r if ltr else list(reversed(r))
    return out
if __name__=='__main__':
    from PIL import Image
    import math
    asc=open('24_ASC.HZK','rb').read(); ar=open('28_AR.HZK','rb').read()
    off=sum(math.ceil(k/8)*2*k*128 for k in range(4,20))
    def cellimg(c):
        if c<0x80:
            im=Image.new('1',(20,40)); g=asc[off+c*120:off+c*120+120]
            for y in range(40):
                for x in range(20):
                    if g[y*3+x//8]&(0x80>>(x%8)): im.putpixel((x,y),1)
            return im
        i=IDX.get(c)
        im=Image.new('1',(40,40))
        if i is None: return im
        g=ar[i*200:i*200+200]
        for y in range(40):
            for x in range(40):
                if g[y*5+x//8]&(0x80>>(x%8)): im.putpixel((x,y),1)
        return im
    lines=['جامعة برج العرب التكنولوجية','قسم تكنولوجيا المعلومات','الامتحان يوم 15 في القاعة B','لا إله إلا الله']
    rows=[]
    for t in lines:
        v=visual(shape(t)); ims=[cellimg(c) for c in v]
        w=sum(i.width for i in ims); row=Image.new('1',(w,40)); x=0
        for i in ims: row.paste(i,(x,0)); x+=i.width
        rows.append(row)
    W=max(r.width for r in rows); big=Image.new('L',(W,44*len(rows)),255)
    for k,r in enumerate(rows): big.paste(r.point(lambda p:0 if p else 255).convert('L'),(0,k*44))
    big=big.resize((big.width*2,big.height*2),Image.NEAREST); big.save('preview.png'); print(big.size)
    json.dump({str(k):v for k,v in forms.items()},open('forms.json','w'))
