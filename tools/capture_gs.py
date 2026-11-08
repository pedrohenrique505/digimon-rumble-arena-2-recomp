#!/usr/bin/env python3
"""Capture an opening prefix; keep only complete, VRAM-verified frame boundaries."""
import argparse, hashlib, json, os, platform, shutil, struct, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def finalize(source,dest):
 end=None;counts={}
 with source.open('rb') as stream:
  if stream.read(8)!=b'DRA2GS02':raise ValueError('invalid GS capture header')
  while header:=stream.read(8):
   if len(header)!=8:break
   kind,size=struct.unpack('<II',header)
   if kind>11 or size>16*1024*1024:raise ValueError('invalid GS capture record')
   if len(stream.read(size))!=size:break
   counts[kind]=counts.get(kind,0)+1
   if kind==6:end=stream.tell()
 if end is None:raise ValueError('no complete VRAM reference; capture longer')
 with source.open('rb') as src,dest.open('xb') as dst:
  remaining=end
  while remaining:
   block=src.read(min(remaining,1024*1024));dst.write(block);remaining-=len(block)
 return dict(original_bytes=source.stat().st_size,complete_bytes=end,discarded_tail_bytes=source.stat().st_size-end,record_counts_before_trimming=counts)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--seconds',type=float,default=35);p.add_argument('--inputs',type=Path,help='JSON list: seconds, key, hold (seconds); xdotool focuses only the game PID');p.add_argument('--binary',type=Path,default=ROOT/'build/dra2');a=p.parse_args()
 if not 10<=a.seconds<=120:p.error('duration must be between 10 and 120 seconds')
 iso=os.environ.get('DRA2_ISO')
 if not iso or not Path(iso).is_file():p.error('set DRA2_ISO to the original disc image')
 events=json.loads(a.inputs.read_text()) if a.inputs else []
 for event in events:
  if not isinstance(event.get('seconds'),(int,float)) or not 0<=event['seconds']<a.seconds or event.get('key') not in ['Return','x','Right','Left','Up','Down','z'] or not 0<event.get('hold',.5)<=2:p.error('invalid input event')
 events=sorted(events,key=lambda e:e['seconds']); delivered=[]
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 env=dict(os.environ,PS2X_GS_CAPTURE=str(out/'raw.gs'),PS2X_VU_CAPTURE=str(out/'vu'))
 for key in ['PS2X_NO_RASTER','PS2X_PERF','PS2X_PERF_FRAMES','PS2X_PERF_FRAME_PC']:env.pop(key,None)
 with (out/'stdout.log').open('w') as log:
  proc=subprocess.Popen([str(a.binary.resolve()),str(ROOT/'game/SLUS_210.67')],cwd=ROOT/'game',env=env,stdout=log,stderr=log)
  start=time.monotonic()
  try:
   for event in events:
    delay=start+event['seconds']-time.monotonic()
    if delay>0:
     try:proc.wait(timeout=delay)
     except subprocess.TimeoutExpired:pass
    if proc.poll() is not None:raise RuntimeError('game exited before input')
    windows=subprocess.check_output(['xdotool','search','--onlyvisible','--pid',str(proc.pid),'--name','PS2-Recomp'],text=True).splitlines()
    if len(windows)!=1:raise RuntimeError('game window is not unique')
    subprocess.run(['xdotool','windowfocus','--sync',windows[0]],check=True)
    subprocess.run(['xdotool','keydown',event['key']],check=True)
    delivered.append(dict(event,actual_seconds=time.monotonic()-start))
    try:time.sleep(event.get('hold',.5))
    finally:subprocess.run(['xdotool','keyup',event['key']],check=True)
   try:proc.wait(timeout=max(.01,start+a.seconds-time.monotonic()))
   except subprocess.TimeoutExpired:pass
  finally:
   proc.terminate()
   try:proc.wait(timeout=3)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
 metadata=finalize(out/'raw.gs',out/'complete.gs')
 def sha(path):
  with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
 metadata.update(binary_sha256=sha(a.binary),elf_sha256=sha(ROOT/'game/SLUS_210.67'),capture_sha256=sha(out/'complete.gs'),machine=platform.uname()._asdict(),scenario='scripted acquisition; see inputs; acquisition overhead is not performance',inputs=delivered,duration_requested=a.seconds,returncode=proc.returncode,vu_files=[f.name for f in sorted(out.glob('*.vu'))])
 (out/'metadata.json').write_text(json.dumps(metadata,indent=2));print(json.dumps(metadata,indent=2))
if __name__=='__main__':main()
