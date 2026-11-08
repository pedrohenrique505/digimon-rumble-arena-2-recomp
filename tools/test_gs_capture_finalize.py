#!/usr/bin/env python3
"""Check capture-version validation and trimming without running the game."""
import struct
import tempfile
from pathlib import Path
from capture_gs import finalize


def record(kind, data):
    return struct.pack('<II', kind, len(data)) + data


with tempfile.TemporaryDirectory(prefix='dra2-finalize-') as directory:
    root = Path(directory)
    for version, count in [(2, 7), (3, 8)]:
        magic = f'DRA2GS0{version}'.encode()
        complete = (magic + record(0, bytes(4 * 1024 * 1024))
                    + record(5, bytes(count * 8)) + record(6, bytes(4 * 1024 * 1024)))
        raw = complete + record(5, bytes(count * 8))[:-3]
        source, dest = root / f'v{version}.raw', root / f'v{version}.gs'
        source.write_bytes(raw)
        meta = finalize(source, dest)
        assert meta['capture_format'] == magic.decode()
        assert dest.read_bytes() == complete and source.read_bytes() == raw
        assert meta['discarded_tail_bytes'] == len(raw) - len(complete)
        source.write_bytes(magic + record(5, bytes((15 - count) * 8)))
        try:
            finalize(source, root / f'bad{version}.gs')
        except ValueError as error:
            assert 'invalid latch size' in str(error)
        else:
            raise AssertionError('version/latch mismatch accepted')
    print('V2/V3 capture trimming and version guards OK')
