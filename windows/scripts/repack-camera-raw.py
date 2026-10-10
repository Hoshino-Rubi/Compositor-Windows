"""Repack upstream's CRT3 xz planes as qCompress data for native Qt resources.
Run with Python 3 from the repository root after updating CameraRawTables.bin.
The table bytes remain identical; Python is a build-maintenance tool only.
"""
import hashlib, lzma, struct, zlib
from pathlib import Path
root = Path(__file__).resolve().parents[2]
source = root / 'Compositor/Resources/CameraRawTables.bin'
data = source.read_bytes()
assert data[:4] == b'CRT3'
size, count, step = struct.unpack_from('<HHH', data, 4)
assert (size, count) == (17, 763) and step > 0
planes = lzma.decompress(data[10:])
assert len(planes) == count * size ** 3 * 3
payload = data[:10] + struct.pack('>I', len(planes)) + zlib.compress(planes, 9)
(root / 'windows/resources/CameraRawTables.qz').write_bytes(payload)
(root / 'windows/resources/CameraRawTables.sha256').write_text(hashlib.sha256(data).hexdigest() + '\n', encoding='ascii')
print('Repacked', len(payload), 'bytes')
