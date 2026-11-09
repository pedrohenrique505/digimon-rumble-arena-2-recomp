#!/usr/bin/env python3
"""Synthetic GIF regression: GS triangle top-left fill rule, subpixel XYOFFSET, and integer pixel sampling."""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path


def record(kind, payload=b''):
    return struct.pack('<II', kind, len(payload)) + payload


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def read_ct32_page0(vram: bytes, x: int, y: int) -> bytes:
    # Within block 0 (8x8 pixels, x < 64, y < 32), PSMCT32 pixel address is swizzled by GSMem::addrPSMCT32.
    block_x = x // 8
    block_y = y // 8
    block_table = (
        (0, 1, 4, 5, 16, 17, 20, 21),
        (2, 3, 6, 7, 18, 19, 22, 23),
        (8, 9, 12, 13, 24, 25, 28, 29),
        (10, 11, 14, 15, 26, 27, 30, 31),
    )
    column = (y & 7) // 2
    sub_y = y & 1
    sub_x = x & 7
    word_in_column = sub_y * 8 + sub_x
    # PSMCT32 column table within a 64-word block:
    col_table = (
        (0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15),
        (16, 17, 20, 21, 24, 25, 28, 29, 18, 19, 22, 23, 26, 27, 30, 31),
        (32, 33, 36, 37, 40, 41, 44, 45, 34, 35, 38, 39, 42, 43, 46, 47),
        (48, 49, 52, 53, 56, 57, 60, 61, 50, 51, 54, 55, 58, 59, 62, 63),
    )
    block = block_table[block_y][block_x]
    word = block * 64 + col_table[column][word_in_column]
    off = word * 4
    return vram[off:off + 4]


def make_shared_edge_capture(rounding: int, fractional_offset: bool) -> bytes:
    # Framebuffer at FBP=0, FBW=1, PSMCT32; ZBUF at ZBP=32 (physically separate).
    ofx = 165 if fractional_offset else 160
    ofy = 331 if fractional_offset else 320
    result = b'DRA2GS03' + record(0, bytes(4 * 1024 * 1024))
    result += record(11, struct.pack('<I', rounding))
    # ALPHA_1: (Cs - 0) * FIX / 128 + Cd with FIX=0x80 -> exact additive Cs + Cd.
    alpha_add = (0 << 0) | (2 << 2) | (2 << 4) | (1 << 6) | (0x80 << 32)
    registers = [
        (0x1a, 1),  # PRMODECONT = 1 (use PRIM)
        (0x18, ofx | (ofy << 32)),
        (0x40, (31 << 16) | (31 << 48)),  # SCISSOR_1: 0..31 x 0..31
        (0x4c, 1 << 16),  # FRAME_1: FBP=0, FBW=1, PSMCT32
        (0x4e, 32 | (1 << 32)),  # ZBUF_1: ZBP=32, ZMSK=1
        (0x47, 0x30000),  # TEST_1: ZTE=1, ZTST=ALWAYS
        (0x42, alpha_add),  # ALPHA_1
        (0x45, 0),  # DTHE = 0
        (0x46, 1),  # COLCLAMP = 1
    ]
    setup = struct.pack('<QQ', len(registers) | (1 << 60), 0x0e)
    setup += b''.join(struct.pack('<QQ', val, reg) for reg, val in registers)
    result += record(1, setup)

    # PRIM: TRIANGLE (3) | ABE (1 << 6)
    prim = 3 | (1 << 6)
    # Two triangles forming window quad [4, 12) x [4, 12), sharing diagonal (4,4)->(12,12).
    verts = [
        (4, 4), (12, 4), (12, 12),
        (4, 4), (12, 12), (4, 12),
    ]
    packet = struct.pack('<QQ', len(verts) | (1 << 46) | (prim << 47) | (2 << 60), 0x51)
    for wx, wy in verts:
        packet += struct.pack('<QQ', 32 | (64 << 32), 96 | (128 << 32))  # RGBA = (32, 64, 96, 128)
        packet += struct.pack('<QQ', (wx * 16 + ofx) | ((wy * 16 + ofy) << 32), 0)
    return result + record(1, packet)


def make_bilinear_half_vertex_capture(rounding: int) -> bytes:
    # Fresh 16x16 PSMCT32 texture at TBP0=4096 (1 MiB, separate from FBP=0).
    # Row v=2 is transparent black (0,0,0,0); rows v=3..4 are (200, 100, 50, 128).
    tex_rows = []
    for y in range(16):
        for x in range(16):
            if y in (3, 4):
                tex_rows.append(bytes((200, 100, 50, 128)))
            else:
                tex_rows.append(bytes((0, 0, 0, 0)))
    texture = b''.join(tex_rows)

    result = b'DRA2GS03' + record(0, bytes(4 * 1024 * 1024))
    result += record(3, struct.pack('<QQQQ', (4096 << 32) | (1 << 48), 0, 16 | (16 << 32), 0))
    result += record(4, texture)
    result += record(11, struct.pack('<I', rounding))

    ofx = 64 * 16  # 64.0
    ofy = 64 * 16  # 64.0
    registers = [
        (0x1a, 1),  # PRMODECONT = 1
        (0x18, ofx | (ofy << 32)),
        (0x40, (31 << 16) | (31 << 48)),
        (0x4c, 1 << 16),  # FRAME_1: FBP=0, FBW=1, PSMCT32
        (0x4e, 32 | (1 << 32)),  # ZBUF_1: ZBP=32, ZMSK=1
        (0x47, 0x30000),  # TEST_1: ZTE=1, ZTST=ALWAYS
        (0x45, 0),
        (0x46, 1),
        # TEX0_1: TBP0=4096, TBW=1, PSMCT32, TW=4 (16), TH=4 (16), TCC=1, TFX=DECAL (1)
        (0x06, 4096 | (1 << 14) | (4 << 26) | (4 << 30) | (1 << 34) | (1 << 35)),
        # TEX1_1: MMAG=1 (linear), MMIN=1 (linear)
        (0x14, (1 << 5) | (1 << 6)),
        (0x08, 5),  # CLAMP_1: clamp S and T
    ]
    setup = struct.pack('<QQ', len(registers) | (1 << 60), 0x0e)
    setup += b''.join(struct.pack('<QQ', val, reg) for reg, val in registers)
    result += record(1, setup)

    # PRIM: TRIANGLE (3) | TME (1 << 4), FST=0 (STQ)
    prim = 3 | (1 << 4)
    # Triangle with half-integer Y vertices:
    # v0=(2.0, 2.5), ST=(2.5/16, 3.0/16)
    # v1=(10.0, 2.5), ST=(10.5/16, 3.0/16)
    # v2=(2.0, 6.5), ST=(2.5/16, 7.0/16)
    verts = [
        (2.0, 2.5, 2.5 / 16.0, 3.0 / 16.0),
        (10.0, 2.5, 10.5 / 16.0, 3.0 / 16.0),
        (2.0, 6.5, 2.5 / 16.0, 7.0 / 16.0),
    ]
    packet = struct.pack('<QQ', 3 | (1 << 46) | (prim << 47) | (3 << 60), 0x512)
    for wx, wy, s, t in verts:
        packet += struct.pack('<QQ', f32_bits(s) | (f32_bits(t) << 32), f32_bits(1.0))
        packet += struct.pack('<QQ', 128 | (128 << 32), 128 | (128 << 32))
        qx = int(round(wx * 16)) + ofx
        qy = int(round(wy * 16)) + ofy
        packet += struct.pack('<QQ', qx | (qy << 32), 0)
    return result + record(1, packet)


def run_case(binary: Path, source: Path, prefix: Path) -> bytes:
    result = subprocess.run(
        [str(binary.resolve()), str(source), str(prefix)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != 2:
        raise RuntimeError(result.stderr or result.stdout)
    vram = Path(str(prefix) + '.vram').read_bytes()
    if len(vram) != 4 * 1024 * 1024:
        raise RuntimeError('incomplete VRAM export')
    return vram


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--dump-bilinear-case', type=Path, default=None,
                        help='optional path to write a single synthetic bilinear triangle capture and exit')
    args = parser.parse_args()

    if args.dump_bilinear_case is not None:
        args.dump_bilinear_case.parent.mkdir(parents=True, exist_ok=True)
        args.dump_bilinear_case.write_bytes(make_bilinear_half_vertex_capture(0))
        return

    failures = []
    cases = 0
    with tempfile.TemporaryDirectory(prefix='dra2-tri-test-') as directory:
        root = Path(directory)
        for rounding in (0, 0x400, 0x800, 0xc00):
            for tag, frac in (('shared-edge', False), ('subpixel-xyoffset', True)):
                name = f'{rounding:#x}-{tag}'
                source, prefix = root / f'{name}.gs', root / name
                source.write_bytes(make_shared_edge_capture(rounding, frac))
                vram = run_case(args.binary, source, prefix)
                for y in range(2, 14):
                    for x in range(2, 14):
                        expected = bytes((32, 64, 96, 128)) if (4 <= x < 12 and 4 <= y < 12) else bytes((0, 0, 0, 0))
                        actual = read_ct32_page0(vram, x, y)
                        if actual != expected:
                            failures.append(f'{name} pixel ({x},{y}): {actual.hex()} != {expected.hex()}')
                cases += 1

            # Half-integer vertex Y + bilinear texel-center alignment at integer XY
            name = f'{rounding:#x}-bilinear-half-vtx'
            source, prefix = root / f'{name}.gs', root / name
            source.write_bytes(make_bilinear_half_vertex_capture(rounding))
            vram = run_case(args.binary, source, prefix)
            for px, py, expected in (
                (3, 2, bytes((0, 0, 0, 0))),
                (3, 3, bytes((200, 100, 50, 128))),
                (2, 3, bytes((200, 100, 50, 128))),
            ):
                actual = read_ct32_page0(vram, px, py)
                if actual != expected:
                    failures.append(f'{name} pixel ({px},{py}): {actual.hex()} != {expected.hex()}')
            cases += 1

    if failures:
        raise SystemExit('\n'.join(failures[:24]) + (f'\n... ({len(failures)} total failures)' if len(failures) > 24 else ''))
    print(f'{cases} GIF triangle rasterization cases passed')


if __name__ == '__main__':
    main()
