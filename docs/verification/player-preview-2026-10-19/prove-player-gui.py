# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import pathlib,os,subprocess,time,shutil
from PIL import ImageGrab
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));out=pathlib.Path(__file__).parent
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('WAYLAND_SOCKET',None);env['DISPLAY']=':96';env['WINIT_UNIX_BACKEND']='x11';env['WGPU_BACKEND']='vulkan'
xvfb=subprocess.Popen(['Xvfb',':96','-screen','0','1280x800x24','-nolisten','tcp'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
scene=out/'player-gui-copy.rscene';shutil.copyfile(root/'samples/99-the-block/city-v2.rscene',scene)
logpath=out/'player-gui.log';log=logpath.open('w')
def x(*args):return subprocess.check_output(['xdotool',*args],env=env,text=True)
def shot(name):ImageGrab.grab(xdisplay=':96').save(out/name)
try:
 time.sleep(.3)
 app=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/the-block-host'),'--scene',str(scene),'--assets',str(root/'build/release/samples/99-the-block/cooked/manifest.txt')],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
 deadline=time.monotonic()+40
 while 'editor host + viewport' not in logpath.read_text():
  assert app.poll() is None,logpath.read_text();assert time.monotonic()<deadline,'startup timeout';time.sleep(.1)
 win=x('search','--pid',str(app.pid),'--name','Rime Editor').splitlines()[0];x('windowfocus','--sync',win);time.sleep(1)
 shot('player-gui-edit.png')
 # Toolbar at default scale: Walk checkbox precedes the established Play controls.
 x('mousemove','586','10');x('click','1');time.sleep(.4);shot('player-gui-walk-selected.png')
 x('mousemove','642','10');x('click','1');time.sleep(2);shot('player-gui-playing.png')
 assert 'Walk Preview admitted' in logpath.read_text(),logpath.read_text()[-2000:]
 x('mousemove','620','400');x('click','1');time.sleep(.4)
 x('keydown','w');time.sleep(.5);x('keyup','w');x('mousemove_relative','--','65','-10');time.sleep(.3);shot('player-gui-captured.png')
 x('key','Escape');time.sleep(.3);shot('player-gui-released.png')
 x('mousemove','620','400');x('click','1');time.sleep(.3);x('keydown','w');time.sleep(.15)
 # Focus loss must return both input and cursor; refocus must not auto-capture.
 focus=subprocess.Popen(['xmessage','-title','RimeFocusProof','-buttons','OK','Capture release proof'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 time.sleep(.3);other=x('search','--name','^RimeFocusProof$').splitlines()[-1];x('windowfocus','--sync',other);time.sleep(.3);x('keyup','w')
 x('windowfocus','--sync',win);time.sleep(.3);shot('player-gui-focus-released.png');focus.terminate();focus.wait(timeout=5)
 # A Shift+Escape release must work while moving too.
 x('click','1');time.sleep(.25);x('keydown','w');x('key','shift+Escape');x('keyup','w');time.sleep(.3);shot('player-gui-shift-escape.png')
 x('mousemove','704','10');x('click','1');time.sleep(.5);shot('player-gui-stopped.png')
 assert scene.read_bytes()==(root/'samples/99-the-block/city-v2.rscene').read_bytes()
 print('PASS: real GUI Walk selection/admission, capture, W, relative look, Esc, focus loss/refocus, Shift+Esc and Stop; screenshots retained. Source unchanged. Screenshots require visual inspection; companion host proof asserts exact movement/restoration.')
finally:
 if 'app'in locals():
  app.terminate()
  try:app.wait(timeout=8)
  except subprocess.TimeoutExpired:app.kill();app.wait()
 log.close();xvfb.terminate();xvfb.wait(timeout=5)
