# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Real Vulkan editor-host wire proof; no fake host or product CLI changes."""
import pathlib, socket, struct, subprocess, threading, time, json, shutil, os, re
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3])); out=pathlib.Path(__file__).parent
scene=out/'player-host-copy.rscene'; shutil.copyfile(root/'samples/99-the-block/city-v2.rscene',scene)
sockpath=str(out/'player.sock'); pathlib.Path(sockpath).unlink(missing_ok=True)
log=(out/'player-host.log').open('w'); state={}; lock=threading.Lock(); seen={}; seq=0
app=subprocess.Popen([os.environ.get('RIME_EDITOR_HOST',str(root/'build/release/bin/the-block-host')),'--editor-host',sockpath,'--viewport','--scene',str(scene),'--assets',str(root/'build/release/samples/99-the-block/cooked/manifest.txt')],cwd=root,stdout=log,stderr=subprocess.STDOUT)
def wait(pred,desc,timeout=35):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if pred(): return
  assert app.poll() is None, (desc,'host exited',app.returncode)
  time.sleep(.02)
 raise AssertionError((desc, state, (out/'player-host.log').read_text()[-1500:]))
def exact(n):
 b=b''
 while len(b)<n:
  q=s.recv(n-len(b))
  if not q: raise EOFError()
  b+=q
 return b
def recv():
 try:
  while True:
   ty,n=struct.unpack('<HI',exact(6)); p=exact(n)
   with lock:
    seen[ty]=seen.get(ty,0)+1
    if ty==0x20b:
     caps,mode,phase,status,cap,sid,tick=struct.unpack_from('<5BQQ',p)
     state.update(caps=caps,mode=mode,phase=phase,status=status,captured=bool(cap),session=sid,tick=tick,reason=p[25:].decode())
    elif ty==0x205: state['eye']=struct.unpack_from('<3f',p,128)
    elif ty==0x201: state['snapshot']=p
    elif ty==0x207: state['save']=p
    elif ty==0x208: state['edit_result']=p
 except (EOFError,OSError): pass
def send(ty,p=b''): s.sendall(struct.pack('<HI',ty,len(p))+p)
def ctl(action,sid=None): send(0x21e,struct.pack('<BQ',action,state['session'] if sid is None else sid))
def inp(kind,code=0,x=0,y=0):
 global seq
 seq+=1;send(0x101,struct.pack('<BIiiffIQI',kind,code,x,y,0,0,0,int(time.monotonic()*1e6),seq))
def ticks(n):
 start=state['tick'];wait(lambda:state['tick']>=start+n,'advance '+str(n)+' ticks')
def snapshot():
 before=seen.get(0x201,0);send(0x215);wait(lambda:seen.get(0x201,0)>before,'snapshot');return state['snapshot']
def eye():
 # Lens is sent after state; wait through two complete rendered frames.
 n=seen.get(0x205,0);wait(lambda:seen.get(0x205,0)>n+1,'fresh camera lens');return state['eye']
def close(a,b,eps=.002): return max(abs(x-y) for x,y in zip(a,b))<eps
try:
 wait(lambda:pathlib.Path(sockpath).exists(),'socket'); s=socket.socket(socket.AF_UNIX);s.connect(sockpath)
 s.sendall(struct.pack('<IH',0x524d5331,4)); assert exact(6)==struct.pack('<IH',0x524d5331,4)
 threading.Thread(target=recv,daemon=True).start();wait(lambda:'eye'in state and 'caps'in state,'initial telemetry')
 assert state['caps']==3; initial_eye=state['eye']; baseline=snapshot()
 ctl(1);wait(lambda:state['mode']==1,'select Walk');send(0x219)
 wait(lambda:state['status']==1 or state['status']==2,'admit/refuse');assert state['status']==1,state
 ticks(30); spawn=eye();assert abs(spawn[0]+10)<.05 and abs(spawn[2]-2)<.05 and .5<spawn[1]<3,spawn
 # Raw uncaptured and stale-session inputs cannot move the player.
 inp(0,0x1a);ticks(10);uncaptured=eye();assert close(spawn,uncaptured),('uncaptured moved',spawn,uncaptured)
 ctl(2,state['session']-1);ticks(3);assert not state['captured']
 ctl(2);wait(lambda:state['captured'],'capture acknowledgement');inp(2,x=0,y=0);inp(0,0x1a);ticks(35);walk=eye();assert walk[2]<spawn[2]-1,('no walking',spawn,walk)
 inp(0,0x2c);ticks(4);jump=eye();assert jump[1]>walk[1]+.2,('no jump',walk,jump)
 inp(1,0x2c);inp(1,0x1a);ticks(50)
 ctl(3);wait(lambda:not state['captured'],'release');released=eye();ticks(10);assert close(released,eye()),'release must stop horizontal motion'
 # Paused camera stays fixed; resume requires explicit new capture.
 send(0x21a);wait(lambda:state['phase']==2,'pause');t=state['tick'];time.sleep(.2);assert state['tick']==t
 send(0x21b);wait(lambda:state['tick']==t+1,'single step');time.sleep(.1);assert state['tick']==t+1 and state['phase']==2
 send(0x219);wait(lambda:state['phase']==1,'resume');assert not state['captured'];inp(0,0x1a);ticks(8);assert close(released,eye()),'resume must not recapture'
 # Server enforces save refusal and structural edit refusal while walking.
 send(0x21d,struct.pack('<I',0));wait(lambda:'save'in state,'save refusal');assert state['save'][0]==0
 send(0x211,struct.pack('<Q',0));wait(lambda:'edit_result'in state,'spawn refusal');assert state['edit_result'][0]==0
 send(0x21c);wait(lambda:state['phase']==0,'Stop restore');assert snapshot()==baseline,'Stop must restore every serialized component'
 # Edit is idle; no second lens is guaranteed. Latest Stop lens equals source camera.
 wait(lambda:close(state['eye'],initial_eye),'restore source camera')
 # Refused Begin must restore the edited baseline, then allow ordinary Inspect.
 spawn_hash=int(re.search(r'rime::block_play::PlayerSpawn 0x([0-9a-f]+)',scene.read_text()).group(1),16)
 cursor=8; spawn_id=None
 for _ in range(struct.unpack_from('<I',baseline,4)[0]):
  _,_,eid,count=struct.unpack_from('<IIQH',baseline,cursor);cursor+=18
  for _ in range(count):
   ty,size=struct.unpack_from('<QI',baseline,cursor);cursor+=12
   if ty==spawn_hash:spawn_id=eid
   cursor+=size
 assert spawn_id is not None
 send(0x210,struct.pack('<QQI',spawn_id,spawn_hash,1)+b'\0');edited=snapshot()
 send(0x219);wait(lambda:state['status']==2,'missing spawn refusal')
 assert state['phase']==0 and not state['captured'] and state['reason'],state
 assert snapshot()==edited,'refused Begin must restore edited baseline'
 ctl(0);wait(lambda:state['mode']==0 and state['status']==0,'Inspect after refusal')
 send(0x219);wait(lambda:state['phase']==1,'Inspect begins');ticks(3)
 send(0x21c);wait(lambda:state['phase']==0,'Inspect Stop');assert snapshot()==edited
 assert scene.read_bytes()==(root/'samples/99-the-block/city-v2.rscene').read_bytes(),'preview changed document'
 print(json.dumps(dict(result='PASS',initial_eye=initial_eye,settled_spawn=spawn,walk=walk,jump=jump,released=released,checks=['real city admission','actual ground support','uncaptured/stale capture ignored','WASD walking','jump','release clears movement','Pause and single Step','Resume requires recapture','Save and spawn refused','Stop exact snapshot restore','source file unchanged','refused Begin exact baseline restore','Inspect available after refusal'],frames=seen.get(1,0)),indent=2))
 send(0xffff)
finally:
 app.terminate()
 try: app.wait(timeout=8)
 except subprocess.TimeoutExpired:app.kill();app.wait()
 log.close();pathlib.Path(sockpath).unlink(missing_ok=True)
