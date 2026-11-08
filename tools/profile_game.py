#!/usr/bin/env python3
"""Amostra pilhas do jogo com REA/GDB; artefatos ficam fora do git."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--binary', type=Path, default=ROOT / 'build/dra2')
parser.add_argument('--samples', type=int, default=60)
parser.add_argument('--interval', type=float, default=0.5)
args = parser.parse_args()
if args.samples < 2 or args.interval <= 0 or args.samples * args.interval > 45:
    parser.error('use ao menos 2 amostras, intervalo positivo e duração total <=45s')
elf = ROOT / 'game/SLUS_210.67'
with elf.open('rb') as source:
    elf_hash = hashlib.file_digest(source, 'sha256').hexdigest()
if elf_hash != '590f453c7e39faeec8a2e46352b2655a18d1eb6e12fe693fe75583efcd15dacd':
    parser.error('ELF diferente do DRA2 NTSC-U v1.02; contador não validado')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
if (out / 'samples.json').exists():
    parser.error('escolha outra pasta para preservar a coleta anterior')
with args.binary.open('rb') as source:
    binary_hash = hashlib.file_digest(source, 'sha256').hexdigest()
(out / 'identity.json').write_text(json.dumps(dict(binary=str(args.binary.resolve()),
    binary_sha256=binary_hash, elf_sha256=elf_hash), indent=2))
# GDB aceita strings com aspas; json.dumps preserva caminhos com espaços.
script = f'''set pagination off
set confirm off
set debuginfod enabled off
set print thread-events off
python import os
file {json.dumps(str(args.binary.resolve()))}
python os.chdir({str(ROOT / 'game')!r})
set args {json.dumps(str(ROOT / 'game/SLUS_210.67'))}
start
python
import gdb, threading, os, signal, collections, json, time
counts=collections.Counter()
stacks=[]
class CaptureRam(gdb.Breakpoint):
    ram = None
    def stop(self):
        self.ram = int(gdb.parse_and_eval('$rdi'))
        self.enabled = False
        return False
capture = CaptureRam('FUN_003a2460_0x3a2460')
started=time.monotonic()
for i in range({args.samples}):
    pid=gdb.selected_inferior().pid
    timer=threading.Timer({args.interval!r}, lambda: os.kill(pid, signal.SIGINT))
    timer.start()
    try:
        gdb.execute('continue', to_string=True)
    finally:
        timer.cancel()
        timer.join()
    if not gdb.selected_inferior().pid:
        raise RuntimeError('jogo terminou antes da coleta')
    sample=[]
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        frames=[]
        f=gdb.newest_frame()
        while f and len(frames)<24:
            frames.append(f.name() or hex(f.pc()))
            f=f.older()
        sample.append(dict(thread=thread.name,frames=frames))
        if thread.name == 'GameThread':
            counts[frames[0] if frames else '?']+=1
    counter = None
    if capture.ram is not None:
        counter = int.from_bytes(gdb.selected_inferior().read_memory(capture.ram + 0x538930, 4).tobytes(), 'little')
    stacks.append(dict(seconds=time.monotonic()-started, frame_counter=counter, threads=sample))
json.dump(dict(elapsed=time.monotonic()-started, samples=stacks, game_leaf=counts),open({str(out / 'samples.json')!r},'w'),indent=2)
end
kill
'''
(out / 'sample.gdb').write_text(script)
proc = subprocess.Popen(['npx', '-y', 'rea-agents@6.8.0', 'mcp'],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=(out / 'rea-stderr.log').open('w'), text=True)
sequence = 0
def call(method, params):
    global sequence
    sequence += 1
    proc.stdin.write(json.dumps(dict(jsonrpc='2.0', id=sequence, method=method, params=params)) + '\n')
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError('REA desconectou')
        response = json.loads(line)
        if response.get('id') == sequence:
            return response

def tool(name, arguments):
    result = call('tools/call', dict(name=name, arguments=arguments))
    (out / (name + '.json')).write_text(json.dumps(result, indent=2))
    if 'error' in result or result.get('result', {}).get('isError'):
        raise RuntimeError(f'REA: falha em {name}; veja {out}')
    return result['result']

session = None
try:
    call('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                        'clientInfo': {'name': 'dra2-performance', 'version': '1'}})
    proc.stdin.write(json.dumps(dict(jsonrpc='2.0', method='notifications/initialized')) + '\n')
    proc.stdin.flush()
    result = tool('start_gdb_session', {})
    data = result.get('structuredContent') or json.loads(result['content'][0]['text'])
    session = data.get('session_id', data.get('result', {}).get('session_id'))
    if not session:
        raise RuntimeError('REA não retornou uma sessão GDB')
    tool('gdb_console', {'session_id': session, 'command': f'source {out / "sample.gdb"}'})
    data = json.loads((out / 'samples.json').read_text())
    counters = [(s['seconds'], s['frame_counter']) for s in data['samples']
                if s['frame_counter'] is not None]
    fps = None
    if len(counters) > 1:
        fps = ((counters[-1][1] - counters[0][1]) & 0xFFFFFFFF) / (counters[-1][0] - counters[0][0])
    if fps is None:
        raise RuntimeError('contador de quadros não observado; amostras preservadas')
    summary = dict(elapsed=data['elapsed'], guest_frames_per_second=fps, game_leaf=data['game_leaf'])
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
finally:
    if session:
        tool('close_gdb_session', {'session_id': session})
    proc.stdin.close()
    proc.wait(timeout=15)
