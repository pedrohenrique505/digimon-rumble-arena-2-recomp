#!/usr/bin/env python3
"""Exercise capture framing, strict CPU equivalence and native image uploads."""
import json
import struct
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

binary = Path(sys.argv[1]).resolve()
gpu = '--gpu' in sys.argv[2:]
VRAM = 4 * 1024 * 1024

def record(kind, payload=b''):
    return struct.pack('<II', kind, len(payload)) + payload

with tempfile.TemporaryDirectory(prefix='dra2-capture-test-') as directory:
    root = Path(directory)
    initial = bytes(VRAM)
    image = bytearray(initial)
    image[:4] = b'\x11\x22\x33\x44'
    upload = record(3, struct.pack('<QQQQ', 1 << 48, 0, 1 | (1 << 32), 0)) + record(4, bytes(image[:4]) + bytes(12))
    valid = b'DRA2GS02' + record(0, initial) + record(10) + record(11, struct.pack('<I', 0)) + upload + record(6, image)
    tests = [('v3-native-upload', valid.replace(b'DRA2GS02', b'DRA2GS03', 1), 0), ('native-upload', valid, 0), ('reference-mismatch', valid[:-1] + b'\x01', 0 if gpu else 1),
             ('truncated-payload', valid[:-3], 1), ('bad-header', b'BADMAGIC', 1),
             ('missing-vram', b'DRA2GS02', 1),
             ('oversized-record', b'DRA2GS02' + struct.pack('<II', 0, 0xffffffff), 1),
             ('orphan-upload', b'DRA2GS02' + record(0, initial) + record(4, bytes(16)), 1)]
    direct = record(9, struct.pack('<IIIIII', 0, 0, 1, 0, 0, 0x88776655))
    direct_image = bytearray(initial)
    direct_image[:4] = b'\x55\x66\x77\x88'
    # A GPU upload followed by a masked 24-bit host write must retain alpha.
    masked = record(9, struct.pack('<IIIIII', 1, 0, 1, 0, 0, 0x00ccbbaa))
    masked_image = bytearray(image)
    masked_image[:3] = b'\xaa\xbb\xcc'
    wide_image = bytearray(initial)
    # Four CT32 pixels exercise actual GPU dispatch before a masked host write.
    for offset in (0, 4, 16, 20):
        wide_image[offset:offset+4] = b'\x11\x22\x33\x44'
    wide_upload = record(3, struct.pack('<QQQQ', 1 << 48, 0, 4 | (1 << 32), 0)) + record(4, b'\x11\x22\x33\x44' * 4)
    wide_image[:3] = b'\xaa\xbb\xcc'
    odd24_image = bytearray(initial)
    for offset in (0, 4, 16):
        odd24_image[offset:offset+3] = b'\x11\x22\x33'
    odd24_upload = record(3, struct.pack('<QQQQ', (1 << 48) | (1 << 56), 0, 3 | (1 << 32), 0)) + record(4, b'\x11\x22\x33' * 3 + bytes(7))
    # Local-to-host FIFO: partial byte consumption and sub-qword tail.
    def reg(addr, value):
        return record(2, struct.pack('<QQ', addr, value))
    fifo_upload = record(3, struct.pack('<QQQQ', 1 << 48, 0, 5 | (1 << 32), 0)) + record(4, b'\x11\x22\x33\x44' * 5)
    fifo_xfer = reg(0x50, 1 << 16) + reg(0x51, 0) + reg(0x52, 5 | (1 << 32)) + reg(0x53, 1)
    fifo_image = bytearray(initial)
    for offset in (0, 4, 16, 20, 32):
        fifo_image[offset:offset+4] = b'\x11\x22\x33\x44'
    def gif_image(bitbltbuf, trxreg, payload):
        setup = struct.pack('<QQ', 4 | (1 << 60), 0xE)
        for value, addr in ((bitbltbuf, 0x50), (0, 0x51), (trxreg, 0x52), (0, 0x53)):
            setup += struct.pack('<QQ', value, addr)
        return record(1, setup + struct.pack('<QQ', 1 | (2 << 58), 0) + payload)
    tests += [('gif-image-partial', b'DRA2GS02' + record(0, initial) + gif_image(1 << 48, 1 | (1 << 32), b'\x11\x22\x33\x44' + bytes(12)) + record(6, image), 0),
              ('gif-image-odd24', b'DRA2GS02' + record(0, initial) + gif_image((1 << 48) | (1 << 56), 3 | (1 << 32), b'\x11\x22\x33' * 3 + bytes(7)) + record(6, odd24_image), 0),
              ('fifo-partial-and-tail', b'DRA2GS02' + record(0, initial) + fifo_upload + fifo_xfer + record(8, struct.pack('<I', 6)) + record(8, struct.pack('<I', 14)) + record(6, fifo_image), 0),
              ('fifo-tail-only', b'DRA2GS02' + record(0, initial) + record(3, struct.pack('<QQQQ', 1 << 48, 0, 3 | (1 << 32), 0)) + record(4, b'\x11\x22\x33\x44' * 3) + fifo_xfer.replace(struct.pack('<Q', 5 | (1 << 32)), struct.pack('<Q', 3 | (1 << 32))) + record(8, struct.pack('<I', 5)) + record(8, struct.pack('<I', 7)) + record(6, bytes(bytearray(b'\x11\x22\x33\x44' * 2 + bytes(8) + b'\x11\x22\x33\x44') + bytes(VRAM - 20))), 0),
              ('odd-24bit-upload', b'DRA2GS02' + record(0, initial) + odd24_upload + record(6, odd24_image), 0),
              ('gpu-upload-then-masked-write', b'DRA2GS02' + record(0, initial) + wide_upload + masked + record(6, wide_image), 0),
              ('direct-write', b'DRA2GS02' + record(0, initial) + direct + record(6, direct_image), 0),
              ('upload-then-masked-write', b'DRA2GS02' + record(0, initial) + upload + masked + record(6, masked_image), 0)]
    expected = {'v3-native-upload': image, 'odd-24bit-upload': odd24_image, 'gpu-upload-then-masked-write': wide_image, 'native-upload': image, 'direct-write': direct_image,
                'gif-image-partial': image, 'gif-image-odd24': odd24_image,
                'fifo-partial-and-tail': fifo_image, 'fifo-tail-only': bytearray(b'\x11\x22\x33\x44' * 2 + bytes(8) + b'\x11\x22\x33\x44') + bytes(VRAM - 20),
                'upload-then-masked-write': masked_image}
    for name, data, code in tests:
        capture = root / (name + '.gs')
        capture.write_bytes(data)
        result = subprocess.run([str(binary), str(capture), str(root / name)] + (['--checkpoints'] if name == 'native-upload' else []), capture_output=True, text=True)
        assert result.returncode == code, (name, result.returncode, result.stdout, result.stderr)
        if name == 'reference-mismatch':
            assert json.loads(result.stdout)['mismatching_references'] == 1
        if name == 'native-upload':
            assert (root / (name + '.checkpoints') / '1.ppm').is_file()
        if code == 0 and name != 'reference-mismatch':
            output = json.loads(result.stdout)
            assert output['references'] == 1 and output['different_bytes'] == 0, (name, output, (root / (name + '.vram')).read_bytes()[:32].hex())
            assert (root / (name + '.vram')).read_bytes() == bytes(expected[name])
        print(name, 'OK')
        for artifact in root.glob(name + '.*'):
            if artifact.is_dir():
                shutil.rmtree(artifact)
            else:
                artifact.unlink()
