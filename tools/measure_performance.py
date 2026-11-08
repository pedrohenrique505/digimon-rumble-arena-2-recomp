#!/usr/bin/env python3
"""Measure the autoload warning without debugger, after a fixed warm-up."""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def card():
    base = ROOT / 'game/mc0'
    return {str(p.relative_to(base)): sha(p) for p in sorted(base.rglob('*')) if p.is_file()}


def read_records(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def summarize(frames, timing, frame_start, frame_end):
    samples = [s for s in frames if frame_start <= s['guest_counter'] <= frame_end]
    if len(samples) != frame_end - frame_start + 1:
        raise RuntimeError('requested guest frame window was not fully observed')
    first_ns, last_ns = samples[0]['wall_ns'], samples[-1]['wall_ns']
    if len(samples) < 20:
        raise RuntimeError('too few render submits in the steady-state window')
    if any(((b['guest_counter'] - a['guest_counter']) & 0xffffffff) != 1
           for a, b in zip(samples, samples[1:])):
        raise RuntimeError('frame hook does not match the guest counter one-to-one')
    intervals = [(b['wall_ns'] - a['wall_ns']) / 1e6 for a, b in zip(samples, samples[1:])]
    span = (samples[-1]['wall_ns'] - samples[0]['wall_ns']) / 1e9
    result = dict(render_submits_per_second=(len(samples) - 1) / span,
                  submit_interval_ms_median=statistics.median(intervals),
                  submit_interval_ms_p95=sorted(intervals)[int(.95 * (len(intervals) - 1))],
                  simulation_seconds_per_real_second=(samples[-1]['ee_cycle'] - samples[0]['ee_cycle']) / 294912000 / span,
                  observed_submits=len(samples), window_seconds=span, guest_frame_window=[frame_start, frame_end])
    costs = [s for s in timing if first_ns <= s['wall_ns'] <= last_ns]
    if len(costs) >= 4:
        elapsed = (costs[-1]['wall_ns'] - costs[0]['wall_ns']) / 1e9
        result['cost_window_seconds'] = elapsed
        result['exclusive_costs'] = {}
        for kind in costs[-1]['exclusive']:
            before, after = costs[0]['exclusive'][kind], costs[-1]['exclusive'][kind]
            result['exclusive_costs'][kind] = {key: after[key] - before[key] for key in before}
        result['window_swaps'] = result['exclusive_costs']['present']['calls']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/dra2')
    parser.add_argument('--pairs', type=int, default=5)
    parser.add_argument('--compare', choices=['matrix', 'raster', 'trace', 'probe'], default='matrix')
    parser.add_argument('--frame-start', type=int, default=200)
    parser.add_argument('--frame-end', type=int, default=260)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    if args.pairs < 1 or args.frame_start < 180 or args.frame_end - args.frame_start < 60 or args.timeout <= 0:
        parser.error('use positive pairs/timeout, first frame >=180 and at least 60 frame intervals')
    iso = os.environ.get('DRA2_ISO')
    if not iso or not Path(iso).is_file():
        parser.error('set DRA2_ISO to the original disc image')
    elf_hash = sha(ROOT / 'game/SLUS_210.67')
    if elf_hash != '590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd':
        parser.error('ELF differs from the Ghidra-validated version')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    metadata = dict(binary=str(args.binary.resolve()), binary_sha256=sha(args.binary), elf_sha256=elf_hash,
                    iso_sha256=sha(Path(iso)), machine=platform.uname()._asdict(),
                    cpu=Path('/proc/cpuinfo').read_text(),
                    revision=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                    dirty=subprocess.check_output(['git', '-C', str(ROOT), 'status', '--short'], text=True),
                    card_initial=card(), scenario='autoload warning, no input, fixed guest frame window',
                    guest_frame_window=[args.frame_start,args.frame_end], timeout_seconds=args.timeout,
                    compare=args.compare, tracing='only DRA2_TRACE_GS/RPC; compiled aggressive logs remain unchanged')
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    variants = {'matrix': ['baseline', 'trace', 'no-raster', 'no-probe'],
                'raster': ['baseline', 'no-raster'], 'trace': ['baseline', 'trace'],
                'probe': ['baseline', 'no-probe']}[args.compare]
    results = []
    for pair in range(args.pairs):
        for variant in (variants if pair % 2 == 0 else list(reversed(variants))):
            run = out / f'{pair}-{variant}'
            run.mkdir()
            env = dict(os.environ, PS2X_PERF=str(run / 'timing.jsonl'), PS2X_PERF_COUNTER='0x538930',
                       PS2X_PERF_FRAME_PC='0x3a2460', PS2X_PERF_FRAMES=str(run / 'frames.jsonl'))
            for key in ['PS2X_NO_RASTER', 'PS2X_GS_CAPTURE', 'PS2X_VU_CAPTURE', 'DRA2_TRACE_GS', 'DRA2_TRACE_RPC']:
                env.pop(key, None)
            if variant == 'no-probe':
                env.pop('PS2X_PERF', None)
            elif variant == 'no-raster':
                env['PS2X_NO_RASTER'] = '1'
            elif variant == 'trace':
                env.update(DRA2_TRACE_GS='1', DRA2_TRACE_RPC='1')
            before = card()
            with (run / 'stdout.log').open('w') as log:
                started = time.monotonic_ns()
                process = subprocess.Popen([str(args.binary.resolve()), str(ROOT / 'game/SLUS_210.67')],
                                           cwd=ROOT / 'game', env=env, stdout=log, stderr=log)
                try:
                    while process.poll() is None and time.monotonic_ns()-started < args.timeout*1e9:
                        time.sleep(.1)
                        records=read_records(run/'frames.jsonl')
                        if records and records[-1]['guest_counter'] >= args.frame_end:
                            break
                    else:
                        raise RuntimeError('game exited or frame window timed out')
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            result = summarize(read_records(run / 'frames.jsonl'), read_records(run / 'timing.jsonl'),
                               args.frame_start, args.frame_end)
            result.update(pair=pair, variant=variant, card_unchanged=before == card())
            if not result['card_unchanged']:
                raise RuntimeError('memory card changed; results invalid')
            results.append(result)
            (run / 'summary.json').write_text(json.dumps(result, indent=2))
            (out / 'results.json').write_text(json.dumps(results, indent=2))
            print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
