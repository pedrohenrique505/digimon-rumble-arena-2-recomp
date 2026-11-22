#!/usr/bin/env python3
"""Check an animated scene in a log captured with DRA2_TRACE_SCANOUT=1."""
import argparse
from pathlib import Path
import re
import sys

SCANOUT = re.compile(r"\[dra2-scanout\] us=(\d+) frame=\d+ size=(\d+x\d+) hash=([0-9a-f]+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start", type=float, required=True, help="seconds from first scanout")
    parser.add_argument("--end", type=float, required=True, help="seconds from first scanout")
    parser.add_argument("--max-freeze-ms", type=float, default=250)
    args = parser.parse_args()
    if not 0 <= args.start < args.end or not args.max_freeze_ms > 0:
        parser.error("expected 0 <= start < end and a positive freeze threshold")

    frames = [(int(us), (size, digest)) for us, size, digest in
              SCANOUT.findall(args.log.read_text(errors="replace"))]
    duration = (frames[-1][0] - frames[0][0]) / 1e6 if frames else 0
    if duration < args.end:
        parser.error("scanout log does not cover the requested interval")

    origin = frames[0][0]
    start = origin + args.start * 1e6
    end = origin + args.end * 1e6
    previous = None
    last_change = start
    max_freeze = 0
    published = changes = 0
    for us, image in frames:
        if us >= end:
            break
        if us >= start:
            published += 1
            if image != previous:
                changes += 1
                max_freeze = max(max_freeze, us - last_change)
                last_change = us
        previous = image

    window = args.end - args.start
    max_freeze_ms = max(max_freeze, end - last_change) / 1e3
    print(f"{published / window:.1f} published/s, {changes / window:.1f} changes/s, "
          f"longest hold {max_freeze_ms:.1f} ms")
    return int(max_freeze_ms > args.max_freeze_ms)


if __name__ == "__main__":
    sys.exit(main())
