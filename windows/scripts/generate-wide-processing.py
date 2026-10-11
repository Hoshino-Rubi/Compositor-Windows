"""Generate the 16-bit implementations sharing the 8-bit algorithms and settings.
Checked-in outputs do not require Python at build/run time. Tests verify non-8-bit
pixel values through each implementation. User parameter ranges remain unchanged.
"""
from pathlib import Path
import re,hashlib
root=Path(__file__).resolve().parents[2];src=root/'windows/src'
renames={'Renderer':['render','renderMask','layerSurface','blendPixel'], 'Effects':['applyEffects','validateEffects'],
         'Filters':['filtered','blurImage','validateAdjustment','supportedAdjustment','alphaPath','wandPath'],
         'CameraRaw':['cameraRaw','normalizedCameraRaw','curveValue','cameraRawHistogram','validateCameraRaw','autoWhiteBalance'],
         'Warp':['warpDab','perspectiveWarp']}
allnames=['render','renderMask','layerSurface','blendPixel','applyEffects','filtered','blurImage','cameraRaw','warpDab','perspectiveWarp','alphaPath','wandPath']
for name,defs in renames.items():
 original=(src/(name+'.cpp')).read_text(encoding="utf-8");code=original
 if name=='CameraRaw':code=code.split('QJsonObject autoWhiteBalance(')[0]+'} // namespace comp\n'
 # Strip dispatch guards from their own specialization.
 code=re.sub(r'    if \(wideImage\([^\n]+\n        return [^\n]+\n','',code)
 code=re.sub(r'    if \(s.bitDepth == 16\)\n        return [^\n]+\n','',code)
 for n in set(allnames+defs):code=re.sub(r'\b'+n+r'\b',n+'16',code)
 code=code.replace('QImage::Format_ARGB32_Premultiplied','QImage::Format_RGBA64_Premultiplied').replace('QImage::Format_RGBA8888_Premultiplied','QImage::Format_RGBA64_Premultiplied').replace('QImage::Format_ARGB32','QImage::Format_RGBA64').replace('QImage::Format_Grayscale8','QImage::Format_Grayscale16')
 code=code.replace('QRgb','QRgba64').replace('qRgba(', 'rgba64(')
 for a,b in [('qRed','wideRed'),('qGreen','wideGreen'),('qBlue','wideBlue'),('qAlpha','wideAlpha')]:code=code.replace(a+'(',b+'(')
 if name=='Renderer':code=code.replace('l.mask.convertToFormat(QImage::Format_Grayscale16)', 'maskImage(l.mask,16)')
 if name in ['Renderer','Effects']:
  code=re.sub(r'\b255\b','65535',code)
  code=code.replace('const auto *src = gray.constScanLine(y);','const auto *src = reinterpret_cast<const quint16 *>(gray.constScanLine(y));')
  code=code.replace('QColor(0, 0, 0, qRound(l.opacity * 65535))','coverageColor(l.opacity)').replace('QColor(0, 0, 0, qRound(groupOpacity * 65535))','coverageColor(groupOpacity)')
 if name=='Filters':
  code=code.replace('rgba.pixel(point)', 'pixel64(rgba,point.x(),point.y())').replace('wideAlpha(row[x]) > 127', 'wideAlpha(row[x]) > 32767').replace('}) <= tolerance;', '}) <= tolerance * 257;')
  code=code.replace('QColor::fromRgba(', 'QColor::fromRgba64(')
  for c in ['red','green','blue']:code=code.replace('c.'+c+'()', 'c.'+c+'F() * 255.')
  code=code.replace('return std::clamp(qRound(v), 0, 255);','return std::clamp(qRound(v * 257.), 0, 65535);')
  code=code.replace('auto *data = rgba.bits();','auto *data = reinterpret_cast<quint16 *>(rgba.bits());').replace('stride = rgba.bytesPerLine();','stride = rgba.bytesPerLine() / 2;')
  code=code.replace('scanlines_apply(rgba.bits(), rgba.width(), rgba.height(), rgba.bytesPerLine(),','wide::scanlines_apply(reinterpret_cast<quint16 *>(rgba.bits()), rgba.width(), rgba.height(), rgba.bytesPerLine() / 2,')
  code=code.replace('dither_glow(rgba.bits(), bloom.constBits(), rgba.width(), rgba.height(), rgba.bytesPerLine(),','wide::dither_glow(reinterpret_cast<quint16 *>(rgba.bits()), reinterpret_cast<const quint16 *>(bloom.constBits()), rgba.width(), rgba.height(), rgba.bytesPerLine() / 2,')
  code=code.replace('uint8_t table[768];','QVector<quint16> table(65536 * 3);').replace('i < 256; ++i)','i < 65536; ++i)').replace('uint8_t(std::clamp(qRound(255 *','quint16(std::clamp(qRound(65535 *').replace('* i / 255.)),','* i / 65535.)),').replace('0, 255));','0, 65535));').replace('stride, table);','stride, table.constData());')
  for n in ['adjust_black_white','adjust_color_balance','adjust_gradient_map','adjust_grain','noise_add']:code=code.replace(n+'(', 'wide::'+n+'(')
 if name=='CameraRaw':
  code=code.replace('auto *pixels = result.bits();','auto *pixels = reinterpret_cast<quint16 *>(result.bits());').replace('stride = result.bytesPerLine();','stride = result.bytesPerLine() / 2;')
  for n in ['adjust_camera_raw_calibration','adjust_camera_raw','adjust_camera_raw_sharpen_mask_overlay','adjust_camera_raw_curve_color','adjust_camera_raw_effects','adjust_grain','adjust_camera_raw_detail','adjust_camera_raw_optics','adjust_camera_raw_clip_overlay']:
   code=code.replace(n+'(', 'wide::'+n+'(')
 if name=='Warp':
  for which in ['image','source']:code=re.sub(which+r'\.pixel\(([^,]+), ([^)]+)\)',r'pixel64('+which+r', \1, \2)',code)
  code=code.replace('mix(16), mix(8), mix(0), mix(24)','mix(0), mix(1), mix(2), mix(3)')
  # The scalar bilinear sampler originally extracts 8-bit channels by packed shifts.
  code=code.replace('((a >> shift) & 255)', '(shift == 0 ? a.red() : shift == 1 ? a.green() : shift == 2 ? a.blue() : a.alpha())').replace('((b >> shift) & 255)', '(shift == 0 ? b.red() : shift == 1 ? b.green() : shift == 2 ? b.blue() : b.alpha())').replace('((c >> shift) & 255)', '(shift == 0 ? c.red() : shift == 1 ? c.green() : shift == 2 ? c.blue() : c.alpha())').replace('((d >> shift) & 255)', '(shift == 0 ? d.red() : shift == 1 ? d.green() : shift == 2 ? d.blue() : d.alpha())')
 code=code.replace('#include "WideProcessing.h"\n','')
 code='#include "WideProcessing.h"\n#include "WideKernels.h"\n'+code
 (src/('Wide'+name+'.cpp')).write_text(f'// Generated by windows/scripts/generate-wide-processing.py.\n// Source SHA256: {hashlib.sha256(original.encode()).hexdigest()}\n'+code,encoding='utf-8')
