# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,subprocess,time,json,shutil
from PIL import ImageGrab,Image,ImageDraw
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));out=pathlib.Path(__file__).parent
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('WAYLAND_SOCKET',None);capture_display=os.environ.get('RIME_CAPTURE_DISPLAY', ':95');assert capture_display.startswith(':') and capture_display[1:].isdigit();assert not pathlib.Path('/tmp/.X11-unix/X'+capture_display[1:]).exists(), 'capture display already in use';env['DISPLAY']=capture_display;env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/nvidia_icd.json'
scene=out/'recorded-city-center.rscene';text=(root/'build/release/city-v2.rscene').read_text();blocks=text.split('\nentity ');matches=[i for i,b in enumerate(blocks) if 'rime::block_play::PlayerSpawn ' in b];assert len(matches)==1;idx=matches[0];import re;blocks[idx]=re.sub(r'translation \{ x [-+0-9.e]+ y ([-+0-9.e]+) z ([-+0-9.e]+) \}',r'translation { x 22 y \1 z \2 }',blocks[idx],count=1);scene.write_text('\nentity '.join(blocks))
xvfb=subprocess.Popen(['Xvfb',capture_display,'-screen','0','1280x720x24','-nolisten','tcp'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
game=None;rec=None;events=[]
def x(*args):return subprocess.check_output(['xdotool',*args],env=env,text=True)
def shot(name):ImageGrab.grab(xdisplay=capture_display).save(out/(name+'.png'))
def move(dx,dy):
 x('mousemove_relative','--',str(dx),str(dy));events.append({'t':time.monotonic()-start,'dx':dx,'dy':dy})
try:
 time.sleep(.3);assert xvfb.poll() is None, 'private X server failed'
 log=(out/'game-city-before.log').open('w')
 game=subprocess.Popen([str(root/'build/release/bin/the_block'),'--play','--scene',str(scene),'--cooked',str(root/'build/release/samples/99-the-block/cooked'),'--viewmodel-content',str(root/'build/release/samples/99-the-block/viewmodel-cooked')],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
 deadline=time.monotonic()+40
 while True:
  assert game.poll() is None,(out/'game-city-before.log').read_text()
  try:
   wins=x('search','--pid',str(game.pid),'--name','Rime').splitlines()
   if wins:break
  except subprocess.CalledProcessError:pass
  assert time.monotonic()<deadline,'no game window';time.sleep(.2)
 win=wins[0];x('windowfocus','--sync',win);x('mousemove','640','360');time.sleep(3)
 rec=subprocess.Popen(['ffmpeg','-y','-loglevel','warning','-f','x11grab','-framerate','30','-video_size','1280x720','-i',capture_display+'.0','-c:v','libx264','-preset','veryfast','-crf','18','-pix_fmt','yuv420p',str(out/'ground-city-before.mp4')],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=(out/'ffmpeg-city-before.log').open('w'))
 start=time.monotonic();shot('city-before-00-level');time.sleep(1)
 # Look down slowly, then pure yaw sweeps, then short walks at the same ground-facing pitch.
 for _ in range(12):move(0,20);time.sleep(.04)
 time.sleep(1);shot('city-before-01-ground')
 for j,direction in enumerate([1,-1,1,-1]):
  for _ in range(20):move(direction*6,0);time.sleep(.035)
  time.sleep(.25);shot('city-before-0'+str(j+2)+'-turn')
 x('keydown','w');events.append({'t':time.monotonic()-start,'key':'w down'});time.sleep(.6);x('keyup','w');events.append({'t':time.monotonic()-start,'key':'w up'});time.sleep(.5);shot('city-before-06-walk')
 for _ in range(20):move(-6,0);time.sleep(.035)
 x('keydown','s');time.sleep(.4);x('keyup','s');time.sleep(.7);shot('city-before-07-back')
 rec.communicate(b'q',timeout=10);assert rec.returncode==0,rec.returncode;rec=None
 x('key','Escape');
 try:game.wait(timeout=2)
 except subprocess.TimeoutExpired:game.terminate();game.wait(timeout=10)
 game=None
 (out/'input-events-city-before.json').write_text(json.dumps(events,indent=2)+'\n')
 images=sorted(out.glob('city-before-0*.png'));sheet=Image.new('RGB',(640*2,360*((len(images)+1)//2)))
 for i,p in enumerate(images):
  im=Image.open(p);im.thumbnail((640,360));sheet.paste(im,((i%2)*640,(i//2)*360));ImageDraw.Draw(sheet).text(((i%2)*640+8,(i//2)*360+8),p.stem,fill='yellow')
 sheet.save(out/'city-before-contact.png');print('Recorded real Run game, ground-facing turns and walking:',out/'ground-city-before.mp4',flush=True)
finally:
 if rec is not None:
  rec.communicate(b'q',timeout=10)
 if game is not None:
  game.terminate();game.wait(timeout=10)
 xvfb.terminate();xvfb.wait(timeout=5)
