# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,subprocess,time,re,shutil,json,math
from PIL import ImageGrab
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));out=pathlib.Path(__file__).parent
scene=out/'picker-copy.rscene';shutil.copyfile(out/'yard-baseline.rscene',scene)
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('WAYLAND_SOCKET',None)
env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['WINIT_UNIX_BACKEND']='x11';env['WGPU_BACKEND']='vulkan'
logfile=out/'picker-gui.log'
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
 app=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/thermal-courtyard'),'--scene',str(scene)],cwd=root,env=env,stdout=stream,stderr=subprocess.STDOUT)
 try:
  wait(lambda:'editor host + viewport' in logfile.read_text())
  window=subprocess.check_output(['xdotool','search','--pid',str(app.pid),'--name','Rime Editor'],env=env,text=True).splitlines()[0]
  xdo('windowfocus','--sync',window);time.sleep(1);click(500,300)
  def reference():
   for block in re.findall(r'(?ms)^entity \d+ \{\n.*?^\}',scene.read_text()):
    if 'rime::fluids_scene::ThermalSource ' in block and 'power_watts -100000' in block:
     return int(re.search(r'cell @(\d+)',block).group(1))
   raise AssertionError('Cooling source missing')
  original=reference()
  click(82,60);click(135,129);xdo('key','Escape');time.sleep(.2)
  click(100,177);click(1100,190);capture().save(out/'picker-options.png')
  click(1100,282);save();selected=reference();capture().save(out/'picker-selected.png')
  assert selected!=original,('Dropdown did not change reference',original,selected)
  click(139,10);click(165,35);save();undone=reference();assert undone==original
  click(139,10);click(165,58);save();redone=reference();assert redone==selected
  text=scene.read_text();assert 'rime::render::MeshRef' not in text and 'rime::render::MaterialRef' not in text
  capture().save(out/'picker-redone.png')
  result=dict(original=original,selected=selected,undone=undone,redone=redone)
  (out/'picker-proof.json').write_text(json.dumps(result,indent=2)+'\n')
  print('Real courtyard GUI: typed entity dropdown selection, exact undo/redo and save without renderer ids PASS',result,flush=True)
 finally:
  app.terminate()
  try:app.wait(timeout=10)
  except subprocess.TimeoutExpired:app.kill();app.wait()
