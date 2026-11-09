#!/usr/bin/env python3
"""Compare complete GS prefixes sequentially; keep game data outside git."""
import argparse
import collections
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path, help='new directory, normally under .cache/')
    parser.add_argument('--build', type=Path, default=Path('.cache/gs-replay-build'))
    parser.add_argument(
        '--start-record',
        type=int,
        default=0,
        help='verify the last testable prefix before N is equal, then scan from record N',
    )
    args = parser.parse_args()
    source = args.capture.read_bytes()
    if source[:8] not in (b'DRA2GS02', b'DRA2GS03'):
        parser.error('invalid capture header')
    records = []
    offset = 8
    pending_upload = False
    while offset < len(source):
        if len(source) - offset < 8:
            parser.error('truncated record header')
        kind, size = struct.unpack_from('<II', source, offset)
        end = offset + 8 + size
        if end > len(source):
            parser.error('truncated record payload')
        if pending_upload != (kind == 4):
            parser.error('native upload header/payload must be adjacent')
        pending_upload = kind == 3
        records.append((kind, size, end))
        offset = end
    if pending_upload or not records or records[0][:2] != (0, 4 * 1024 * 1024):
        parser.error('missing initial VRAM or incomplete native upload')
    if args.start_record < 0 or args.start_record >= len(records):
        parser.error(f'--start-record must be in 0..{len(records) - 1}')
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'capture_sha256': hashlib.sha256(source).hexdigest(),
              'previous_equal_record': None, 'first_difference': None}

    def replay_prefix(tmp: Path, index: int, end: int):
        capture = tmp / 'prefix.gs'
        capture.write_bytes(source[:end])
        arrays = []
        for label, executable in (('cpu', 'dra2-gs-replay'),
                                  ('gpu', 'dra2-gs-replay-vulkan')):
            prefix = tmp / label
            for old in tmp.glob(label + '.*'):
                old.unlink()
            result = subprocess.run([str((args.build / executable).resolve()),
                                     str(capture), str(prefix)],
                                    capture_output=True, text=True, timeout=120)
            # 2 means no reference; CPU may return 1 for an old reference.
            if result.returncode not in (0, 1, 2) or not prefix.with_suffix('.vram').exists():
                raise RuntimeError(f'{label}, record {index}: {result.stderr}')
            array = prefix.with_suffix('.vram').read_bytes()
            if len(array) != 4 * 1024 * 1024:
                raise RuntimeError('invalid replay VRAM size')
            arrays.append(array)
        return arrays

    with tempfile.TemporaryDirectory(dir=args.output) as directory:
        tmp = Path(directory)
        if args.start_record > 0:
            prev_index = args.start_record - 1
            while prev_index >= 0 and records[prev_index][0] == 3:
                prev_index -= 1
            if prev_index < 0:
                raise RuntimeError('no testable prefix before --start-record')
            prev_arrays = replay_prefix(tmp, prev_index, records[prev_index][2])
            if prev_arrays[0] != prev_arrays[1]:
                raise RuntimeError(
                    f'prefix at record {prev_index} before --start-record {args.start_record} already differs'
                )
            report['previous_equal_record'] = prev_index
            print(f'verified equal through record {prev_index}', flush=True)

        for index in range(args.start_record, len(records)):
            kind, size, end = records[index]
            if kind == 3:
                continue  # Upload header and payload form one indivisible prefix.
            arrays = replay_prefix(tmp, index, end)
            if arrays[0] != arrays[1]:
                differences = [i for i, (a, b) in enumerate(zip(*arrays)) if a != b]
                pairs = collections.Counter((arrays[0][i], arrays[1][i]) for i in differences)
                report['first_difference'] = dict(record=index, kind=kind, payload_size=size,
                    different_bytes=len(differences), first_offset=differences[0],
                    last_offset=differences[-1], byte_pairs=[
                        dict(cpu=a, gpu=b, count=count) for (a, b), count in pairs.most_common()])
                (args.output / 'after.gs').write_bytes(source[:end])
                previous = report['previous_equal_record']
                if previous is not None:
                    (args.output / 'before.gs').write_bytes(source[:records[previous][2]])
                for label, array in zip(('cpu', 'gpu'), arrays):
                    (args.output / (label + '.vram')).write_bytes(array)
                break
            report['previous_equal_record'] = index
            if index % 10 == 0:
                print(f'equal through record {index}', flush=True)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
