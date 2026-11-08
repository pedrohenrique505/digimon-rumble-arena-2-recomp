#!/usr/bin/env python3
"""Alternate deterministic CPU/Vulkan replays; keep images and full run evidence."""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
from pathlib import Path


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--build', type=Path, default=Path('.cache/gs-replay-build'))
    parser.add_argument('--pairs', type=int, default=5)
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error('pairs must be positive')
    binaries = {'cpu': (args.build / 'dra2-gs-replay').resolve(),
                'gpu': (args.build / 'dra2-gs-replay-vulkan').resolve()}
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    metadata = dict(capture=str(args.capture.resolve()), capture_sha256=sha(args.capture),
                    binaries={name: dict(path=str(path), sha256=sha(path)) for name, path in binaries.items()},
                    machine=platform.uname()._asdict(), pairs=args.pairs,
                    boundary='post-arbitration GIF to offline scanout, no window swap; seed/init excluded')
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    env = dict(os.environ)
    for key in ['PS2X_PERF', 'PS2X_PERF_FRAMES', 'PS2X_NO_RASTER', 'PS2X_GS_CAPTURE', 'PS2X_VU_CAPTURE']:
        env.pop(key, None)
    results = []
    for pair in range(args.pairs):
        for name in (['cpu', 'gpu'] if pair % 2 == 0 else ['gpu', 'cpu']):
            label = f'{pair}-{name}'
            with (out / (label + '.stderr')).open('w') as error:
                run = subprocess.run([str(binaries[name]), str(args.capture.resolve()), str(out / label)],
                                     stdout=subprocess.PIPE, stderr=error, text=True, env=env, timeout=300)
            (out / (label + '.json')).write_text(run.stdout)
            if run.returncode:
                raise RuntimeError(f'{label} failed with {run.returncode}; evidence preserved')
            data = json.loads(run.stdout)
            if name == 'cpu' and data['mismatching_references']:
                raise RuntimeError('CPU reference diverged; speed comparison is invalid')
            data.update(pair=pair, timed_seconds=data['work_seconds'] + data['sync_seconds'])
            results.append(data)
            (out / 'results.json').write_text(json.dumps(results, indent=2))
            print(json.dumps(data), flush=True)
    medians = {name: statistics.median(r['timed_seconds'] for r in results
                                     if r['backend'] == ('cpu' if name == 'cpu' else 'parallel-gs'))
               for name in binaries}
    summary = dict(median_seconds=medians, gs_replay_speedup=medians['cpu'] / medians['gpu'],
                   visual_equivalence_approved=False, note='compare exported images; speed alone does not approve integration')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
