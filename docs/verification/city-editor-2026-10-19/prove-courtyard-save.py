import pathlib,subprocess,os,time,shutil
root=pathlib.Path('/home/next/projects/rime');out=pathlib.Path(__file__).parent;scene=out/'courtyard-viewport-save.rscene'
shutil.copyfile(root/'samples/thermal-courtyard/courtyard.rscene',scene)
env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json'
for stage in ['save','reopen']:
 log=out/f'courtyard-{stage}.log'
 with log.open('w') as stream:
  app=subprocess.Popen([str(root/'tools/target/debug/editor'),'--engine',str(root/'build/release/bin/thermal-courtyard'),'--scene',str(scene)],env=env,stdout=stream,stderr=subprocess.STDOUT)
  try:
   end=time.monotonic()+30
   while 'editor host + viewport' not in log.read_text():
    assert app.poll() is None,log.read_text()
    assert time.monotonic()<end
    time.sleep(.2)
   assert '155 entities' in log.read_text() and '0 skipped' in log.read_text()
   if stage=='save':
    time.sleep(2)
    window=subprocess.check_output(['xdotool','search','--pid',str(app.pid),'--name','Rime Editor'],env=env,text=True).splitlines()[0]
    subprocess.run(['xdotool','windowfocus','--sync',window],env=env,check=True)
    before=scene.stat().st_mtime_ns
    subprocess.run(['xdotool','key','ctrl+s'],env=env,check=True)
    end=time.monotonic()+30
    while scene.stat().st_mtime_ns==before:
     assert time.monotonic()<end
     time.sleep(.1)
    text=scene.read_text();assert 'rime::render::MeshRef' not in text and 'rime::render::MaterialRef' not in text
   print('Courtyard actual viewport',stage,'passed: 155 entities, 0 skipped, no serialized draw ids',flush=True)
  finally:
   app.terminate()
   try:app.wait(timeout=10)
   except subprocess.TimeoutExpired:app.kill();app.wait()
