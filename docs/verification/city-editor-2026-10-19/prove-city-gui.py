import subprocess,pathlib,os,time,json,shutil
from PIL import Image,ImageGrab
root=pathlib.Path('/home/next/projects/rime');out=pathlib.Path(__file__).parent
scene=out/'city-clean.rscene'; env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None)
env.update(VK_DRIVER_FILES='/usr/share/vulkan/icd.d/lvp_icd.json',WGPU_BACKEND='vulkan',ASAN_OPTIONS='detect_leaks=0')
engine=env.get('CITY_PROOF_HOST',str(root/'build/release/bin/the-block-host'))
label='asan' if engine.endswith('-asan') else 'normal'
def wait_for(predicate,seconds=45):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if predicate():return
  time.sleep(.2)
 raise RuntimeError('proof timed out')
def xdo(*args):subprocess.run(['xdotool',*args],env=env,check=True)
args=[str(root/'tools/target/debug/editor'),'--engine',engine,'--scene',str(scene),'--assets',str(root/'build/release/samples/99-the-block/cooked/manifest.txt'),'--game',str(root/'build/release/bin/the_block')]
image=out/f'gui-{label}-game.ppm'
if image.exists():image.unlink()
for value in ['--play','--frames','4','--ppm',str(image)]:args.extend(['--game-arg',value])
for stage in ['save-run','reopen']:
 logfile=out/f'gui-{label}-{stage}.log'
 with logfile.open('w') as log:
  app=subprocess.Popen(args,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
  try:
   wait_for(lambda: '569 slab previews' in logfile.read_text() or app.poll() is not None)
   assert app.poll() is None,logfile.read_text()
   assert '0 roles missing, 0 unknown patterns' in logfile.read_text()
   time.sleep(3)
   ImageGrab.grab(xdisplay=env['DISPLAY']).save(out/f'gui-{label}-{stage}.png')
   if stage=='save-run':
    window=subprocess.check_output(['xdotool','search','--pid',str(app.pid),'--name','Rime Editor'],env=env,text=True).splitlines()[0]
    xdo('windowfocus','--sync',window)
    xdo('mousemove','500','300','click','1')
    time.sleep(.5)
    before=scene.stat().st_mtime_ns
    xdo('mousemove','106','10','click','1')
    time.sleep(.3)
    xdo('mousemove','125','35','click','1')
    wait_for(lambda:scene.stat().st_mtime_ns!=before)
    text=scene.read_text()
    assert 'rime::render::MeshRef' not in text and 'rime::render::MaterialRef' not in text
    xdo('mousemove','695','10','click','1')
    wait_for(image.exists,90)
    runtime_logs=lambda:list(pathlib.Path('/tmp').glob(f'rime-game-{app.pid}-*.log'))
    wait_for(lambda:any('99-the-block: 4 frames' in p.read_text() for p in runtime_logs()))
    for path in runtime_logs():shutil.copyfile(path,out/f'gui-{label}-runtime.log')
    time.sleep(2)
    assert not any(s in logfile.read_text() for s in ['ERROR: AddressSanitizer','runtime error:'])
    Image.open(image).save(out/f'gui-{label}-game.png')
    ImageGrab.grab(xdisplay=env['DISPLAY']).save(out/f'gui-{label}-returned.png')
    print(label,'real GUI: File Save excluded renderer refs; Run game produced a real window capture',flush=True)
   else:print(label,'fresh GUI reopened saved map, reconstructed all 569 previews, no unknown roles/patterns',flush=True)
  finally:
   app.terminate()
   try:app.wait(timeout=10)
   except subprocess.TimeoutExpired:app.kill();app.wait()
