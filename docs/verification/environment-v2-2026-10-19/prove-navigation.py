# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,subprocess,time,re,shutil,json,math
from PIL import ImageGrab
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));out=pathlib.Path(__file__).parent
scene=out/'navigation-copy.rscene';shutil.copyfile(root/'build/release/city-editor.rscene',scene)
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('WAYLAND_SOCKET',None)
env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['WINIT_UNIX_BACKEND']='x11';env['WGPU_BACKEND']='vulkan'
logfile=out/'navigation-gui.log'
def xdo(*args):subprocess.run(['xdotool',*args],env=env,check=True)
def wait(predicate,seconds=35):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if predicate():return
  assert app.poll() is None,logfile.read_text()
  time.sleep(.1)
 raise AssertionError('Timed out waiting for GUI/engine observation')
def capture():return ImageGrab.grab(xdisplay=env['DISPLAY'])
def phase(color):
 im=capture().crop((275,70,1000,740))
 return sum(1 for px in im.get_flattened_data() if px[:3]==color)>150

def click(x,y):
 xdo('mousemove',str(x),str(y));time.sleep(.15);xdo('click','1');time.sleep(.15)
def pose():
 for block in re.findall(r'(?ms)^entity \d+ \{\n.*?^\}',scene.read_text()):
  if 'rime::render::Camera ' in block and 'active true' in block:
   m=re.search(r'translation \{ x (\S+) y (\S+) z (\S+) \}',block)
   assert m,block
   return [float(x) for x in m.groups()]
 raise AssertionError('Saved camera missing')
def save():
 count=logfile.read_text().count('editor-host: saved ')
 click(106,10);click(125,35)
 wait(lambda:logfile.read_text().count('editor-host: saved ')>count)
 return pose()
def fly(key,seconds):
 xdo('mousemove','640','350');time.sleep(.15);xdo('mousedown','3');time.sleep(.15)
 xdo('keydown',key);time.sleep(seconds);xdo('keyup',key);xdo('mouseup','3');time.sleep(.3)
def distance(a,b):return math.dist(a,b)
with logfile.open('w') as stream:
 app=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/the-block-host'),'--scene',str(scene)],cwd=root,env=env,stdout=stream,stderr=subprocess.STDOUT)
 try:
  wait(lambda:'editor host + viewport' in logfile.read_text())
  window=subprocess.check_output(['xdotool','search','--pid',str(app.pid),'--name','Rime Editor'],env=env,text=True).splitlines()[0]
  xdo('windowfocus','--sync',window);time.sleep(1);click(500,300)
  before=save();capture().save(out/'navigation-before.png')
  click(580,10);wait(lambda:phase((90,200,120)))
  fly('w',1.0);playing=save()
  assert distance(before,playing)>.3,('Play camera snapped back',before,playing)
  time.sleep(.7);released=save()
  assert distance(playing,released)<1e-4,('Release drift/snap',playing,released)
  click(605,10);wait(lambda:phase((220,190,90)))
  fly('d',.4);paused=save();assert distance(paused,playing)>.3
  click(655,10);wait(lambda:not phase((220,190,90)) and not phase((90,200,120)))
  restored=save();assert distance(before,restored)<1e-4,('Stop must restore authored camera',before,restored)
  fly('w',.35);after_stop=save();assert distance(restored,after_stop)>.3
  assert distance(after_stop,restored)<distance(playing,restored),('First post-Stop movement reused stale position',after_stop,playing)
  text=scene.read_text();assert 'rime::render::MeshRef' not in text and 'rime::render::MaterialRef' not in text
  capture().save(out/'navigation-after.png')
  result=dict(before=before,playing=playing,released=released,paused=paused,restored=restored,after_stop=after_stop)
  (out/'navigation-proof.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
  print('Real GUI + city host: Play navigation, release hold, Pause navigation, Stop restoration, post-Stop navigation and clean saved descriptions PASS',flush=True)
 finally:
  app.terminate()
  try:app.wait(timeout=10)
  except subprocess.TimeoutExpired:app.kill();app.wait()
