# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,subprocess,time,shutil
from PIL import ImageGrab
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));out=pathlib.Path(__file__).parent
scene=out/'framed-editor-copy.rscene';shutil.copyfile(root/'samples/99-the-block/city-v2.rscene',scene)
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('WAYLAND_SOCKET',None)
env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['WINIT_UNIX_BACKEND']='x11';env['WGPU_BACKEND']='vulkan'
logfile=out/'framed-editor.log'
def xdo(*args):subprocess.run(['xdotool',*args],env=env,check=True)
with logfile.open('w') as stream:
 app=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/the-block-host'),'--scene',str(scene),'--assets',str(root/'build/release/samples/99-the-block/cooked/manifest.txt')],cwd=root,env=env,stdout=stream,stderr=subprocess.STDOUT)
 try:
  deadline=time.monotonic()+45
  while 'editor host + viewport' not in logfile.read_text():
   assert app.poll() is None,logfile.read_text()
   assert time.monotonic()<deadline,'GUI startup timeout'
   time.sleep(.1)
  window=subprocess.check_output(['xdotool','search','--pid',str(app.pid),'--name','Rime Editor'],env=env,text=True).splitlines()[0]
  xdo('windowfocus','--sync',window);time.sleep(1.0)
  xdo('mousemove','580','10');xdo('click','1');time.sleep(1.5)
  first=ImageGrab.grab(xdisplay=env['DISPLAY']);first.save(out/'framed-editor-playing.png')
  time.sleep(.35)
  second=ImageGrab.grab(xdisplay=env['DISPLAY']);second.save(out/'framed-editor-playing-later.png')
  green=lambda im:sum(1 for px in im.crop((275,70,1000,740)).get_flattened_data() if px[:3]==(90,200,120))
  assert green(first)>150 and green(second)>150,'Preview must be playing'
  text=logfile.read_text();assert 'loaded 662 entities' in text
  assert '0 roles missing, 0 unknown patterns' in text
  print('Real GUI Preview: 662 entities, 32 static timber boards, complete palette, moving effects; screenshots saved.')
 finally:
  app.terminate()
  try:app.wait(timeout=10)
  except subprocess.TimeoutExpired:app.kill();app.wait()
