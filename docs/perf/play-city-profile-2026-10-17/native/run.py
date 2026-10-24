from pathlib import Path
import json,shutil,subprocess,time
root=Path('/home/next/projects/rime');p=Path('/home/next/reports/rime-city-profile-2026-10-17');base=p/'native-final'
def busy():
 rows=subprocess.check_output(['ps','-eo','user=,pid=,comm='],text=True).splitlines()
 return [r for r in rows if r.split()[0]=='ghrunner' and (r.split()[2] in ('ctest','ninja','cc1plus','the_block') or r.split()[2].startswith('rime_'))]
for name,state in [('native-off1','0'),('native-on1','1'),('native-on2','1'),('native-off2','0')]:
 if busy():raise SystemExit('CI is active; refusing native capture')
 (base/'exit.json').unlink(missing_ok=True)
 command='env RIME_VK_DESCRIPTOR_CACHE='+state+' RIME_VK_DESCRIPTOR_STATS=0 python3 /home/next/reports/rime-city-perf-2026-10-17/invoke_capture.py '+str(base)
 subprocess.run(['hyprctl','eval','hl.exec_cmd('+json.dumps(command)+', {float=true, size="1280 720", center=true})'],check=True,stdout=subprocess.DEVNULL)
 deadline=time.clock_gettime(time.CLOCK_BOOTTIME)+120
 while not (base/'exit.json').exists():
  if time.clock_gettime(time.CLOCK_BOOTTIME)>deadline:raise SystemExit(name+' timed out')
  time.sleep(.5)
 status=json.loads((base/'exit.json').read_text());assert status['exit_code']==0,status
 assert not busy(),'CI resumed; capture excluded'
 status['descriptor_cache']=state=='1';(base/'exit.json').write_text(json.dumps(status,indent=2)+'\n')
 subprocess.run(['python3',str(root/'docs/perf/play-city-2026-10-17/analyze.py'),'--root',str(root),'--build-dir',str(root/'build/release'),'--out-dir',str(base)],check=True,stdout=subprocess.DEVNULL)
 d=p/name;d.mkdir(exist_ok=True)
 for f in ['capture.frames.csv','capture.intervals.csv','capture.summary.json','run.log','gpu.csv','exit.json','provenance.json']:shutil.copyfile(base/f,d/f)
 a=json.loads((d/'capture.summary.json').read_text());print(name,{phase:{k:round(a['phases'][phase]['dt_ms'][k],3) for k in ['p50','p99']} for phase in ['hip','ads','turning']},flush=True)
