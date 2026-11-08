#!/usr/bin/env python3
"""Synthetic GIF regression: sample sprite textures at GS integer pixel positions."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path


def record(kind, payload=b''):
    return struct.pack('<II', kind, len(payload)) + payload


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def capture(mode, origin, rounding):
    # Fresh CT32 texture at 1 MiB, physically separate from the framebuffer.
    texture = b''.join(bytes((x * 4, y * 4, 64, 128))
                       for y in range(64) for x in range(64))
    result = b'DRA2GS03' + record(0, bytes(4 * 1024 * 1024))
    result += record(3, struct.pack('<QQQQ', (4096 << 32) | (1 << 48),
                                   0, 64 | (64 << 32), 0))
    result += record(4, texture) + record(11, struct.pack('<I', rounding))
    registers = [(0x1a, 1), (0x18, 0), (0x40, (63 << 16) | (63 << 48)),
                 (0x4c, 1 << 16), (0x4e, 1 << 32), (0x47, 0x30000),
                 (0x45, 0), (0x46, 1),
                 # TEX0: CT32, 64x64, DECAL with texture alpha.
                 (0x06, 4096 | (1 << 14) | (6 << 26) | (6 << 30) |
                  (1 << 34) | (1 << 35)), (0x14, 0), (0x08, 5)]
    setup = struct.pack('<QQ', len(registers) | (1 << 60), 0xe)
    setup += b''.join(struct.pack('<QQ', value, address) for address, value in registers)
    result += record(1, setup)
    prim = 6 | (0 if mode == 'solid' else 16) | (256 if mode == 'uv' else 0)
    packet = struct.pack('<QQ', 2 | (1 << 46) | (prim << 47) | (3 << 60),
                         0x513 if mode == 'uv' else 0x512)
    for x, y in ((0, 0), (32, 32)):
        if mode == 'uv':
            packet += struct.pack('<QQ', int((x + origin) * 16) |
                                  (int((y + origin) * 16) << 32), 0)
        else:
            packet += struct.pack('<QQ', bits((x + origin) / 64) |
                                  (bits((y + origin) / 64) << 32), bits(1))
        packet += struct.pack('<QQ', 128 | (128 << 32), 128 | (128 << 32))
        packet += struct.pack('<QQ', x * 16 | ((y * 16) << 32), 0)
    return result + record(1, packet)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    failures = []
    cases = 0
    with tempfile.TemporaryDirectory(prefix='dra2-sprite-test-') as directory:
        root = Path(directory)
        # glibc/x86 rounding modes, matching the Linux x86-64 capture format.
        for rounding in (0, 0x400, 0x800, 0xc00):
            for mode in ('st', 'uv', 'solid'):
                for origin in (0, 0.5):
                    name = f'{rounding}-{mode}-{int(origin * 2)}'
                    source, prefix = root / (name + '.gs'), root / name
                    source.write_bytes(capture(mode, origin, rounding))
                    result = subprocess.run([str(args.binary.resolve()), str(source), str(prefix)],
                                            capture_output=True, text=True, timeout=30)
                    if result.returncode != 2:  # Synthetic stream has no VRAM reference.
                        raise RuntimeError(result.stderr or result.stdout)
                    vram = Path(str(prefix) + '.vram').read_bytes()
                    if len(vram) != 4 * 1024 * 1024:
                        raise RuntimeError('incomplete VRAM export')
                    # First 2x2 CT32 pixels occupy these four physical words.
                    # Nearest sampling at integer XY with origin 0 or 0.5
                    # must select texel (x,y), never (x+1,y+1).
                    for x, y, offset in ((0, 0, 0), (1, 0, 4), (0, 1, 8), (1, 1, 12)):
                        expected = (bytes((128, 128, 128, 128)) if mode == 'solid'
                                    else bytes((x * 4, y * 4, 64, 128)))
                        actual = vram[offset:offset + 4]
                        if actual != expected:
                            failures.append(f'{name} pixel ({x},{y}): '
                                            f'{actual.hex()} != {expected.hex()}')
                    cases += 1
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'{cases} GIF sprite sampling cases passed')


if __name__ == '__main__':
    main()
