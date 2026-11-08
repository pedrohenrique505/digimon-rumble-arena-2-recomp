#!/usr/bin/env python3
"""Compare the raw DISPFB images exported by two replays with --checkpoints."""
import argparse
import hashlib
import json
from pathlib import Path


def read_image(path):
    data = path.read_bytes()
    header = b'P6\n640 448\n255\n'
    if not data.startswith(header) or len(data) != len(header) + 640 * 448 * 3:
        raise ValueError(f'expected replay PPM, 640x448 RGB: {path}')
    return data[len(header):]


def compare(cpu, gpu, output):
    left = {p.stem for p in cpu.glob('*.ppm')}
    right = {p.stem for p in gpu.glob('*.ppm')}
    if not left or left != right:
        raise ValueError('checkpoint sets must be nonempty and identical')
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for name in sorted(left, key=int):
        a_meta = json.loads((cpu / f'{name}.json').read_text())
        b_meta = json.loads((gpu / f'{name}.json').read_text())
        if any(a_meta[key] != b_meta[key] for key in ('record', 'latch')):
            raise ValueError(f'checkpoint {name}: event positions differ')
        if a_meta['different_bytes']:
            raise ValueError(f'checkpoint {name}: CPU differs from capture reference')
        a, b = read_image(cpu / f'{name}.ppm'), read_image(gpu / f'{name}.ppm')
        delta = bytes(abs(x - y) for x, y in zip(a, b))
        pixels = sum(delta[i:i+3] != b'\0\0\0' for i in range(0, len(delta), 3))
        (output / f'{name}-diff.ppm').write_bytes(b'P6\n640 448\n255\n' + delta)
        results.append(dict(checkpoint=int(name), record=a_meta['record'], latch=a_meta['latch'],
                            different_pixels=pixels, total_pixels=640 * 448,
                            mean_absolute_channel_error=sum(delta) / len(delta),
                            max_channel_error=max(delta), gpu_vram_different_bytes=b_meta['different_bytes'],
                            cpu_rgb_sha256=hashlib.sha256(a).hexdigest(),
                            gpu_rgb_sha256=hashlib.sha256(b).hexdigest()))
    summary = dict(checkpoints=results, raw_display_equal=all(r['different_pixels'] == 0 for r in results),
                   visual_equivalence_approved=False,
                   scope='raw 640x448 DISPFB; excludes scanout, deinterlace and window presentation')
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cpu', type=Path)
    parser.add_argument('gpu', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(compare(args.cpu, args.gpu, args.output), indent=2))


if __name__ == '__main__':
    main()
