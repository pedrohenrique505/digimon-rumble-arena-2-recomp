#!/usr/bin/env python3
"""Verify diagnostic scanout pixels, metadata, CLI guards and missing images."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
gpu = '--gpu' in sys.argv[2:]


def record(kind, payload):
    return struct.pack('<II', kind, len(payload)) + payload


with tempfile.TemporaryDirectory(prefix='dra2-scanout-') as directory:
    root = Path(directory)
    # Uniform VRAM makes this independent of swizzle. NTSC active area begins
    # at DX=636, DY=25; MAGH=3 gives 640 output pixels from 2560 clocks.
    vram = b'\x11\x22\x33\x80' * (1024 * 1024)
    display = 636 | (25 << 12) | (3 << 23) | (2559 << 32) | (223 << 44)
    regs = (1 | (1 << 5) | (1 << 7) | (255 << 8), 0, 10 << 9, display, 0, 0, 0)
    capture = root / 'uniform.gs'
    capture.write_bytes(b'DRA2GS02' + record(0, vram)
                       + record(5, struct.pack('<7Q', *regs)) + record(6, vram))
    prefix = root / 'out'
    command = [str(binary), str(capture), str(prefix), '--scanout-ntsc']
    result = subprocess.run(command, capture_output=True, text=True)
    if gpu:
        assert result.returncode == 0, result.stderr
        meta = json.loads(prefix.with_suffix('.scanout.json').read_text())
        assert meta['latch'] == 1
        assert meta['video_mode_source'] == 'explicit-ntsc-assumption'
        assert meta['visual_equivalence_approved'] is False
        image = prefix.with_suffix('.scanout.ppm').read_bytes()
        header = f"P6\n{meta['width']} {meta['height']}\n255\n".encode()
        assert image.startswith(header)
        pixels = image[len(header):]
        assert meta['width'] == 640 and meta['height'] == 224, meta
        assert pixels == b'\x11\x22\x33' * (640 * 224), (len(pixels), pixels[:24])
        assert json.loads(result.stdout)['scanout_export_seconds'] > 0
        # Existing outputs are never overwritten.
        assert subprocess.run(command, capture_output=True).returncode == 1
        assert prefix.with_suffix('.scanout.ppm').read_bytes() == image
        empty = root / 'no-latch.gs'
        empty.write_bytes(b'DRA2GS02' + record(0, vram) + record(6, vram))
        missing = subprocess.run([str(binary), str(empty), str(root / 'missing'),
                                  '--scanout-ntsc'], capture_output=True, text=True)
        assert missing.returncode == 1 and 'no scanout image' in missing.stderr
    else:
        assert result.returncode == 1 and 'requires the Vulkan replay' in result.stderr
        assert not prefix.with_suffix('.vram').exists()
    print('scanout pixels/metadata/guards OK' if gpu else 'CPU rejects GPU scanout option OK')
