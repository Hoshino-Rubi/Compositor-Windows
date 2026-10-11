import struct,zlib,uuid
from pathlib import Path
out=Path('windows/tests/fixtures');out.mkdir(exist_ok=True)
def u16(n):return struct.pack('>H',n)
def u32(n):return struct.pack('>I',n)
def u64(n):return struct.pack('>Q',n)
def i32(n):return struct.pack('>i',n)
def extra(key,data):return b'8BIM'+key+u32(len(data))+data+(b'\0' if len(data)%2 else b'')
def psd(psb=False,compression=0):
 w,h=8,6;layers=[];channels=[];length=u64 if psb else u32
 for name,rgb,rect,section in [('Top', (20,180,60),(1,2,5,6),0),('Background',(160,30,20),(0,0,h,w),0)]:
  top,left,bottom,right=rect;ww,hh=right-left,bottom-top;planes=[]
  for value in [rgb[0],rgb[1],rgb[2],255]:
   data=bytes([value])*(ww*hh)
   if compression==0:payload=u16(0)+data
   elif compression==1:
    row=bytes([257-ww,value]);payload=u16(1)+b''.join((u32 if psb else u16)(len(row)) for _ in range(hh))+row*hh
   else:payload=u16(compression)+zlib.compress(data if compression==2 else b''.join(bytes([value])+b'\0'*(ww-1) for _ in range(hh)))
   planes.append(payload)
  nameBytes=name.encode();pascal=bytes([len(nameBytes)])+nameBytes;pascal+=b'\0'*((-len(pascal))%4)
  ex=u32(0)+u32(0)+pascal+extra(b'luni',u32(len(name))+name.encode('utf-16be'))
  record=b''.join(i32(v) for v in rect)+u16(4)+b''.join(struct.pack('>h',id)+length(len(payload)) for id,payload in zip([0,1,2,-1],planes))+b'8BIMnorm'+bytes([255,0,0,0])+u32(len(ex))+ex
  layers.append(record);channels.extend(planes)
 info=struct.pack('>h',len(layers))+b''.join(layers)+b''.join(channels);info+=b'\0'*(len(info)%2);layer_section=length(len(info))+info+u32(0)
 return b'8BPS'+u16(2 if psb else 1)+b'\0'*6+u16(3)+u32(h)+u32(w)+u16(8)+u16(3)+u32(0)+u32(0)+length(len(layer_section))+layer_section+u16(0)+bytes([0])*(w*h*3)
for psb in [False,True]:
 for compression in range(4):(out/f'layers-{compression}.{ "psb" if psb else "psd"}').write_bytes(psd(psb,compression))
# Self-generated uncompressed, 16-bit linear DNG (RGB photometric LinearRaw) with no embedded preview.
entries=[];data=bytearray();w,h=32,24
sizes={1:1,2:1,3:2,4:4,5:8,10:8}
def tag(tag,type,values):
 if type==2:payload=values.encode()+b'\0';count=len(payload)
 elif type==1:payload=bytes(values);count=len(values)
 elif type in [3,4]:payload=struct.pack('<'+('H' if type==3 else 'I')*len(values),*values);count=len(values)
 else:payload=b''.join(struct.pack('<ii' if type==10 else '<II',a,b) for a,b in values);count=len(values)
 entries.append((tag,type,count,payload))
tag(254,4,[0]);tag(256,4,[w]);tag(257,4,[h]);tag(258,3,[16,16,16]);tag(259,3,[1]);tag(262,3,[34892]);tag(271,2,'Compositor');tag(272,2,'Generated DNG');tag(273,4,[0]);tag(277,3,[3]);tag(278,4,[h]);tag(279,4,[w*h*6]);tag(284,3,[1]);tag(50706,1,[1,4,0,0]);tag(50707,1,[1,1,0,0]);tag(50708,2,'Compositor Linear Fixture');tag(50717,4,[65535,65535,65535]);tag(50721,10,[(1,1),(0,1),(0,1),(0,1),(1,1),(0,1),(0,1),(0,1),(1,1)]);tag(50728,5,[(1,1),(1,1),(1,1)]);tag(50778,3,[21]);entries.sort();base=8+2+12*len(entries)+4;extraData=bytearray();records=[]
for key,type,count,payload in entries:
 if len(payload)<=4:value=payload.ljust(4,b'\0')
 else:value=struct.pack('<I',base+len(extraData));extraData+=payload;extraData+=b'\0'*(len(extraData)%2)
 records.append([key,type,count,value])
strip=base+len(extraData)
for record in records:
 if record[0]==273:record[3]=struct.pack('<I',strip)
raw=b''.join(struct.pack('<HHH',1000+x*800,1000+y*1000,10000) for y in range(h) for x in range(w))
(out/'linear.dng').write_bytes(b'II*\0'+struct.pack('<I',8)+struct.pack('<H',len(records))+b''.join(struct.pack('<HHI',a,b,c)+d for a,b,c,d in records)+b'\0'*4+extraData+raw)
(out/'vector.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="32"><rect width="48" height="32" fill="#f00"/><circle cx="24" cy="16" r="8" fill="#00f"/></svg>')
print('Generated PSD, PSB raw/RLE/ZIP/predictor, linear DNG and SVG fixtures')
# Additional synthetic Photoshop fixtures: no copyrighted document contents.
def make_layer(name,rect=(0,0,6,8),rgb=None,extras=b'',mask=None,clip=0,section=0):
 planes=[];top,left,bottom,right=rect;ww,hh=right-left,bottom-top
 if rgb is not None:
  for id,value in zip([0,1,2,-1],[*rgb,255]):planes.append((id,u16(0)+bytes([value])*ww*hh))
 mask_data=b''
 if mask is not None:
  mt,ml,mb,mr,values=mask
  mask_data=b''.join(i32(v) for v in [mt,ml,mb,mr])+bytes([0,0])+b'\0\0'
  planes.append((-2,u16(0)+bytes(values)))
 if section:extras+=extra(b'lsct',u32(section))
 pascal=bytes([len(name)])+name.encode();pascal+=b'\0'*((-len(pascal))%4)
 data=u32(len(mask_data))+mask_data+u32(0)+pascal+extras
 record=b''.join(i32(v) for v in rect)+u16(len(planes))+b''.join(struct.pack('>h',id)+u32(len(payload)) for id,payload in planes)+b'8BIMnorm'+bytes([255,clip,0,0])+u32(len(data))+data
 return record,b''.join(payload for _,payload in planes)
def layered(records,name):
 info=struct.pack('>h',len(records))+b''.join(a for a,b in records)+b''.join(b for a,b in records);info+=b'\0'*(len(info)%2)
 section=u32(len(info))+info+u32(0)
 (out/name).write_bytes(b'8BPS'+u16(1)+b'\0'*6+u16(3)+u32(6)+u32(8)+u16(8)+u16(3)+u32(0)+u32(0)+u32(len(section))+section+u16(0)+b'\0'*(8*6*3))
for compression in range(4):
 def outside_layer(name,rect,rgb):
  top,left,bottom,right=rect;ww,hh=right-left,bottom-top
  planes=[]
  for value in [*rgb,255]:
   raw=bytes([value])*ww*hh
   if compression==0:blob=u16(0)+raw
   elif compression==1:
    row=bytes([257-ww,value]);blob=u16(1)+u16(len(row))*hh+row*hh
   else:
    packed=raw if compression==2 else b''.join(bytes([value])+b'\0'*(ww-1) for _ in range(hh))
    blob=u16(compression)+zlib.compress(packed)
   planes.append(blob)
  label=name.encode();pascal=bytes([len(label)])+label;pascal+=b'\0'*((-len(pascal))%4)
  ext=u32(0)+u32(0)+pascal
  record=b''.join(i32(v) for v in rect)+u16(4)+b''.join(struct.pack('>h',channel)+u32(len(blob)) for channel,blob in zip([0,1,2,-1],planes))+b'8BIMnorm'+bytes([255,0,0,0])+u32(len(ext))+ext
  return record,b''.join(planes)
 layered([outside_layer('Outside',(-2,-2,8,10),(20,180,60)),
          outside_layer('Base',(0,0,6,8),(160,30,20))],f'oversize-layer-{compression}.psd')
layered([make_layer('Group',(0,0,0,0),section=1),make_layer('Masked',(0,0,6,8),(180,40,30),mask=(0,0,6,8,[255 if x<4 else 0 for y in range(6) for x in range(8)])),make_layer('End',(0,0,0,0),section=3)],'group-mask.psd')
curves=b'\0'+u16(1)+u16(1)+u16(3)+u16(0)+u16(0)+u16(220)+u16(128)+u16(255)+u16(255)
layered([make_layer('Curve',extras=extra(b'curv',curves)),make_layer('Base',rgb=(80,120,150))],'curves.psd')
def unicode(s):return u32(len(s))+s.encode('utf-16be')
def key(s):b=s.encode();return u32(0 if len(b)==4 else len(b))+b
def descriptor(items):return unicode('')+key('null')+u32(len(items))+b''.join(key(k)+kind+payload for k,kind,payload in items)
fill=descriptor([(k,b'doub',struct.pack('>d',v)) for k,v in [('Rd  ',20),('Grn ',180),('Bl  ',60)]])
# Sharp rectangle vector mask.
knots=[(1,1),(7,1),(7,5),(1,5)];path=u32(3)+u32(0)+u16(0)+u16(4)+b'\0'*22
for x,y in knots:path+=u16(1)+b''.join(i32(round(v*16777216)) for _ in range(3) for v in [y/6,x/8])
layered([make_layer('Shape',(0,0,0,0),extras=extra(b'SoCo',u32(16)+fill)+extra(b'vmsk',path))],'shape.psd')
engine=b'<< /ResourceDict << /FontSet [ << /Name (Arial) >> << /Name (Courier New) >> ] >> /EngineDict << /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /Font 0 /FontSize 24 /FillColor << /Values [ 1 1 0 0 ] >> >> >> >> << /StyleSheet << /StyleSheetData << /Font 1 /FontSize 24 /FillColor << /Values [ 1 0 0 1 ] >> >> >> >> ] /RunLengthArray [ 4 5 ] >> /ParagraphRun << /RunArray [ << /ParagraphSheet << /Properties << /Justification 2 >> >> >> ] >> >> >>'
tysh=u16(1)+struct.pack('>6d',1,0,0,1,0,0)+u16(50)+u32(16)+descriptor([('Txt ',b'TEXT',unicode('Red Blue\r')),('EngineData',b'tdta',u32(len(engine))+engine)])
layered([make_layer('Text',rgb=(180,30,60),extras=extra(b'TySh',tysh))],'text.psd')
for compression in [2,3]:
 raw=bytes([160])*48+bytes([30])*48+bytes([20])*48
 if compression==3:raw=b''.join(bytes([v])+b'\0'*7 for v in [160]*6+[30]*6+[20]*6)
 (out/f'flattened-{compression}.psd').write_bytes(b'8BPS'+u16(1)+b'\0'*6+u16(3)+u32(6)+u32(8)+u16(8)+u16(3)+u32(0)*3+u16(compression)+zlib.compress(raw))

# RGB16 fixtures use per-row 16-bit delta prediction and the global Lr16 block.
def psd16_plane(values,w,h,compression,psb):
 data=b''.join(u16(v) for v in values)
 if compression==0:return u16(0)+data
 if compression==1:
  rows=[]
  for y in range(h):
   raw=data[y*w*2:(y+1)*w*2]
   rows.append(b''.join(bytes([len(raw[x:x+128])-1])+raw[x:x+128] for x in range(0,len(raw),128)))
  return u16(1)+b''.join((u32 if psb else u16)(len(row)) for row in rows)+b''.join(rows)
 if compression==3:
  data=b''.join(u16((values[y*w+x]-(values[y*w+x-1] if x else 0))&65535) for y in range(h) for x in range(w))
 return u16(compression)+zlib.compress(data)

def psd16(psb,compression,flat=False,outside=False):
 w,h=17,9; length=u64 if psb else u32
 values=[[10001+p*31 for p in range(w*h)],[20002+p*29 for p in range(w*h)],[30003+p*23 for p in range(w*h)],[65535]*(w*h)]
 parts=[psd16_plane(v,w,h,compression,psb) for v in values]
 if compression==0:composite=u16(0)+b''.join(p[2:] for p in parts)
 elif compression==1:
  n=h*(4 if psb else 2);composite=u16(1)+b''.join(p[2:2+n] for p in parts)+b''.join(p[2+n:] for p in parts)
 else:composite=u16(compression)+zlib.compress(b''.join(zlib.decompress(p[2:]) for p in parts))
 if flat:section=b''
 else:
  records=[];payloads=[]
  for name,rect,base,masked in [('Top',(-2,-2,11,19) if outside else (2,3,7,10),[12345,23456,34567,40001],True),('Background',(0,0,h,w),[10001,20002,30003,65535],False)]:
   top,left,bottom,right=rect;ww,hh=right-left,bottom-top
   colors=[[base[c]+(p*7 if c<3 else 0) for p in range(ww*hh)] for c in range(4)]
   if masked:colors.append([31001+p for p in range(ww*hh)])
   blobs=[psd16_plane(c,ww,hh,compression,psb) for c in colors]
   channels=b''.join(struct.pack('>h',i if i<3 else -1 if i==3 else -2)+length(len(b)) for i,b in enumerate(blobs))
   bounds=b''.join(i32(v) for v in rect)
   mask=bounds+bytes([255,0])+b'\0\0' if masked else b''
   label=name.encode();pascal=bytes([len(label)])+label;pascal+=b'\0'*((-len(pascal))%4)
   ext=u32(len(mask))+mask+u32(0)+pascal
   records.append(bounds+u16(len(blobs))+channels+b'8BIMnorm'+bytes([255,0,0,0])+u32(len(ext))+ext)
   payloads.extend(blobs)
  info=struct.pack('>h',-2)+b''.join(records)+b''.join(payloads)
  section=length(0)+u32(0)+b'8BIMLr16'+length(len(info))+info+b'\0'*((-len(info))%4)
 return b'8BPS'+u16(2 if psb else 1)+b'\0'*6+u16(4)+u32(h)+u32(w)+u16(16)+u16(3)+u32(0)+u32(0)+length(len(section))+section+composite
for psb in [False,True]:
 for compression in range(4):
  for flat in [False,True]:
   name=f'wide-{ "flat" if flat else "layers"}-{compression}.{ "psb" if psb else "psd"}'
   (out/name).write_bytes(psd16(psb,compression,flat))
  (out/f'wide-oversize-{compression}.{ "psb" if psb else "psd"}').write_bytes(psd16(psb,compression,outside=True))

# Uncompressed RGB16 TIFF with exact low-order bits, independent of Qt's encoder.
def rgb16_tiff():
 w,h=17,9;count=11;extra=8+2+count*12+4;pixels=extra+6
 tags=[(256,4,1,w),(257,4,1,h),(258,3,3,extra),(259,3,1,1),(262,3,1,2),(273,4,1,pixels),(277,3,1,3),(278,4,1,h),(279,4,1,w*h*6),(284,3,1,1),(339,3,1,1)]
 entries=b''.join(struct.pack('<HHI',tag,kind,n)+(struct.pack('<H',value)+b'\0\0' if kind==3 and n==1 else struct.pack('<I',value)) for tag,kind,n,value in tags)
 data=b''.join(struct.pack('<HHH',10001+x*31,20002+y*29,30003+x*23) for y in range(h) for x in range(w))
 return b'II*\0'+struct.pack('<I',8)+struct.pack('<H',count)+entries+b'\0'*4+struct.pack('<HHH',16,16,16)+data
(out/'wide-rgb.tif').write_bytes(rgb16_tiff())
