import subprocess, pathlib, os, time, json, sys
from PIL import ImageGrab
root=pathlib.Path('/home/next/projects/rime');out=pathlib.Path(__file__).parent
number=114
while pathlib.Path(f'/tmp/.X11-unix/X{number}').exists() or pathlib.Path(f'/tmp/.X{number}-lock').exists():number+=1
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('XAUTHORITY',None)
env.update(DISPLAY=f':{number}',VK_DRIVER_FILES='/usr/share/vulkan/icd.d/lvp_icd.json',WGPU_BACKEND='vulkan')
server=subprocess.Popen(['Xvfb',f':{number}','-screen','0','1440x900x24','-ac','-nolisten','tcp'],stdout=(out/'gui-xvfb.log').open('w'),stderr=subprocess.STDOUT)
editor=None
try:
 for _ in range(100):
  if pathlib.Path(f'/tmp/.X11-unix/X{number}').exists():break
  time.sleep(.05)
 editor=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/thermal-courtyard'),'--scene',str(root/'samples/thermal-courtyard/courtyard.rscene')],cwd=root,env=env,stdout=(out/'gui-launch.log').open('w'),stderr=subprocess.STDOUT)
 (out/'gui-process.json').write_text(json.dumps({'display':f':{number}','pid':editor.pid}))
 time.sleep(4)
 if editor.poll() is not None:raise RuntimeError('GUI exited; read gui-launch.log')
 ImageGrab.grab(xdisplay=f':{number}').save(out/'gui-list.png')
 print('GUI ready',number,editor.pid,flush=True)
 for command in sys.stdin:
  if command.strip()=='q':break
  ImageGrab.grab(xdisplay=f':{number}').save(out/('gui-'+command.strip()+'.png'))
  print('captured',command.strip(),flush=True)
finally:
 if editor is not None:
  editor.terminate()
  try:editor.wait(timeout=10)
  except subprocess.TimeoutExpired:editor.kill();editor.wait()
 server.terminate();server.wait(timeout=10)
