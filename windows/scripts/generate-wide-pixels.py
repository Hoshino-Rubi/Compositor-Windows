"""Generate 16-bit variants of the MIT upstream pixel kernels.
Pixel strides are in uint16_t elements; byte-based LUTs remain uint8_t.
Run from the repository root. Outputs are checked in and tested independently.
"""
from pathlib import Path
import re,hashlib
root=Path(__file__).resolve().parents[2]
src=root/'Compositor/Rendering';out=root/'windows/src'

def function(code,name):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[^\n;{}]*\b'+name+r'\([^;]*?\)\s*\{',code);assert m,name
 end=m.end();level=1
 while level:
  if code[end]=='{':level+=1
  elif code[end]=='}':level-=1
  end+=1
 return code[m.start():end]

def widen(code):
 code=code.replace('uint8_t','uint16_t')
 code=re.sub(r'\b255(?=\b|[uUfF])','65535',code)
 return code

names=['AdjustPixels','LensPixels','NoisePixels','DitherPixels']
headers=[]
for name in names:
 h=(src/(name+'.h')).read_text(encoding="utf-8");h=re.sub(r'^#.*\n','',h,flags=re.M);h=re.sub(r'//[^\n]*','',h);h=re.sub(r'\n{3,}','\n\n',h)
 if name=='AdjustPixels':h=re.sub(r'typedef struct \{.*?\} CameraRawStage;', '',h,flags=re.S)
 if name=='DitherPixels':
  h='\n'.join(re.findall(r'(?:void|int)\s+(?:scanlines_apply|dither_glow)\([^;]+;',h,re.S))
 h=widen(h)
 if name=='AdjustPixels':h=h.replace('const uint16_t *exposureTables','const uint8_t *exposureTables').replace('const uint16_t *uncertain','const uint8_t *uncertain')
 headers.append(h)
(out/'WideKernels.h').write_text('#pragma once\n#include <cstdint>\n#include <cstddef>\nextern "C" {\n#include "AdjustPixels.h"\n#include "DitherPixels.h"\n}\n// All wide pixel strides count uint16_t elements, not bytes.\nnamespace comp::wide {\n'+''.join(headers)+'int spot_heal(uint16_t *, const uint8_t *, size_t, size_t, size_t, float, int, uint32_t);\nint patch_fill(uint16_t *, size_t, const uint8_t *, size_t, int, int, uint32_t);\nvoid heal_membrane(float *, const uint8_t *, long, long, int);\n'+'\n}\n',encoding='utf-8')
for name in names:
 original=(src/(name+'.c')).read_text(encoding="utf-8");code=original
 if name=='AdjustPixels':
  protected={n:function(code,n) for n in ['table_lookup','lut_at']}
  for n,t in protected.items():code=code.replace(t,'@@'+n+'@@')
  code=widen(code)
  for n,t in protected.items():code=code.replace('@@'+n+'@@',t)
  code=code.replace('const uint16_t *uncertain', 'const uint8_t *uncertain')
  code=code.replace('camera_raw_stage_color(&', 'comp::wide::camera_raw_stage_color(&')
  code=code.replace('const uint16_t *exposure,','const uint8_t *exposure,').replace('const uint16_t *exposureTables,','const uint8_t *exposureTables,')
  code=code.replace('double decoded[256];','double decoded[65536];').replace('i < 256; ++i) decoded','i < 65536; ++i) decoded')
  code=code.replace('double decoded[65536];\n    for (int i = 0; i < 65536; ++i) decoded[i] = srgb_decode(i / 65535.0);', 'static const std::vector<double> decoded = [] { std::vector<double> table(65536); for (int i=0;i<65536;++i) table[i]=srgb_decode(i/65535.0); return table; }();')
  code=code.replace('const double *decode = decoded;', 'const double *decode = decoded.data();')
  for color in ['red','green','blue']:code=code.replace(color+' * 65535)',color+' * 255)')
  code=code.replace('65535 * rec709','255 * rec709').replace('int threshold = 65535','int threshold = 255')
  code=code.replace('254.5 / 65535.0','65534.5 / 65535.0')
  code=code.replace('127u','32767u')
  code=code.replace('const uint8_t *exposureTables, int tableSize) {', 'const uint8_t *exposureTables, int tableSize) {\n    (void)profileDistortion; (void)scale;')
  code=code.replace('malloc(height * stride)','malloc(height * stride * sizeof(uint16_t))').replace('width * 4);','width * 4 * sizeof(uint16_t));')
  code=code.replace('size_t bytes = height * stride;','size_t bytes = height * stride * sizeof(uint16_t);')
 if name=='LensPixels':code=widen(code)
 if name=='NoisePixels':code=widen(code).replace('127.5f','32767.5f')
 if name=='DitherPixels':
  # Only the native Scanlines filter and its glow are used by the Windows UI.
  selected=['in_bands','clamp01','adjust_tone','write_pixel','dither_glow','draw_lines','scanlines_apply']
  code='\n\n'.join(function(code,n) for n in selected)
  code=widen(code)
  for field in ['dark','light']:
   for k in range(3):code=code.replace(f'p->{field}[{k}] / 65535.0f',f'p->{field}[{k}] / 255.0f')
  code=code.replace('malloc(count);','malloc(count * sizeof(uint16_t));').replace('malloc(width * 4)','malloc(width * 4 * sizeof(uint16_t))').replace('memcpy(copy, row, width * 4);','memcpy(copy, row, width * 4 * sizeof(uint16_t));')
 code=code.replace('M_PI','3.14159265358979323846')
 code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
 code=code.replace('static void in_bands(size_t count, void (^body)(size_t start, size_t end))','template<class Body> static void in_bands(size_t count, Body body)')
 code=code.replace('dispatch_apply(bands, DISPATCH_APPLY_AUTO,','pixel_parallel(bands,').replace('^(','[&](').replace('__block int failed = 0','std::atomic<int> failed{0}')
 # C allocation assignments get explicit C++ casts based on each declared destination.
 code=re.sub(r'\bmalloc\(', '(float *)malloc(',code).replace('calloc(', '(double *)calloc(')
 for var in ['low','weight']:
  code=code.replace(f'*{var} = (double *)calloc(',f'*{var} = (float *)calloc(')
 for var in ['alpha','copy']:
  code=code.replace(f'uint16_t *{var} = (float *)malloc(',f'uint16_t *{var} = (uint16_t *)malloc(')
 banner=f'// Generated by windows/scripts/generate-wide-pixels.py from {name}.c\n// Source SHA256: {hashlib.sha256(original.encode()).hexdigest()}\n'
 (out/('Wide'+name+'.cpp')).write_text(banner+'#include "WideKernels.h"\n#include "PixelParallel.h"\n#define _USE_MATH_DEFINES\n#include <cmath>\n#include <cstdlib>\n#include <cstring>\n#include <algorithm>\n#include <vector>\nnamespace comp::wide {\n'+code+'\n}\n',encoding='utf-8')

# Retouch kernels use byte role/coverage masks but genuine 16-bit RGBA samples.
for name in ['HealPixels']:
 original=(src/(name+'.c')).read_text(encoding="utf-8");code=re.sub(r'^#include[^\n]*\n','',original,flags=re.M)
 for var in (['rgba','t','s','filled','f'] if name=='HealPixels' else ['pixels','a','b']):
  code=re.sub(r'\buint8_t(\s*\*'+var+r'\b)',r'uint16_t\1',code)
 code=code.replace('(uint8_t)lround','(uint16_t)lround').replace('out[3] > 255 ? 255','out[3] > 65535 ? 65535').replace('x*4+3]==255','x*4+3]==65535').replace('(best%w)*4,4);','(best%w)*4,4*sizeof(uint16_t));')
 code=code.replace('malloc(', 'wide_malloc(').replace('calloc(', 'wide_calloc(').replace('M_PI','3.14159265358979323846')
 if name=='HealPixels':
  code=code.replace('wide_malloc(height * stride)', 'wide_malloc(height * stride * sizeof(uint16_t))').replace('memcpy(filled, rgba, height * stride)', 'memcpy(filled, rgba, height * stride * sizeof(uint16_t))')
  code=code.replace('role[y * ww + x] = coverage[(size_t)(wy0 + y) * width + (size_t)(wx0 + x)] ? HOLE : OUTSIDE;', 'role[y * ww + x] = uint8_t(coverage[(size_t)(wy0 + y) * width + (size_t)(wx0 + x)] ? HOLE : OUTSIDE);')
  for decl in ['long ringCount = 0;', 'long ox = 0, oy = 0;', 'int haveSource = 0;', 'double mean[4] = {0, 0, 0, 0}, detail[3] = {0, 0, 0};']:
   assert decl in code,decl
   code=code.replace('    '+decl+'\n','').replace('    int status = -1;','    '+decl+'\n    int status = -1;')
 else:
  code=code.replace('int d=a[c]-b[c];','double d=double(a[c])-b[c];').replace('    uint32_t seed=0x6d2b79f5;\n','').replace('    int radius=', '    uint32_t seed=0x6d2b79f5;\n    int radius=')
 alloc='struct Allocation { void *p; template<class T> operator T *() const { return static_cast<T *>(p); } };\nstatic Allocation wide_malloc(size_t n) { return {std::malloc(n)}; }\nstatic Allocation wide_calloc(size_t n, size_t s) { return {std::calloc(n,s)}; }\n'
 banner=f'// Generated by windows/scripts/generate-wide-pixels.py from {name}.c\n// Source SHA256: {hashlib.sha256(original.encode()).hexdigest()}\n'
 (out/('Wide'+name+'.cpp')).write_text(banner+'#include "WideKernels.h"\n#include <cmath>\n#include <cstdlib>\n#include <cstring>\n#include <float.h>\nnamespace comp::wide {\n'+alloc+code+'\n}\n',encoding='utf-8')
