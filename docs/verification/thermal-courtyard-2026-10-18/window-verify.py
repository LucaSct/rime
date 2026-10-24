import pathlib,subprocess,os,time
out=pathlib.Path(__file__).parent;root=pathlib.Path('/home/next/projects/rime')
number=114
while pathlib.Path(f'/tmp/.X11-unix/X{number}').exists() or pathlib.Path(f'/tmp/.X{number}-lock').exists():number+=1
with (out/'xvfb.log').open('w') as log:
 server=subprocess.Popen(['Xvfb',f':{number}','-screen','0','1280x720x24','-ac','-nolisten','tcp'],stdout=log,stderr=log)
 try:
  for _ in range(100):
   if server.poll() is not None:raise RuntimeError('Own Xvfb exited')
   if pathlib.Path(f'/tmp/.X11-unix/X{number}').exists():break
   time.sleep(.05)
  env=os.environ.copy();env.pop('WAYLAND_DISPLAY',None);env.pop('XAUTHORITY',None)
  env.update(DISPLAY=f':{number}',VK_DRIVER_FILES='/usr/share/vulkan/icd.d/lvp_icd.json',RIME_REQUIRE_VULKAN='1')
  with (out/'window-x11.log').open('w') as log:
   subprocess.run([str(root/'build/release/bin/thermal-courtyard'),'--scene',str(root/'samples/thermal-courtyard/courtyard.rscene'),'--frames','8','--capture',str(out/'window-x11.ppm')],cwd=root,env=env,stdout=log,stderr=log,check=True)
  if 'presenting in a' not in (out/'window-x11.log').read_text():raise RuntimeError('Not a real window')
  with (out/'forced-x11-input.log').open('w') as log:
   subprocess.run(['ctest','--test-dir','build/release','--output-on-failure','-R','^first_light_windowed_input$'],cwd=root,env=env,stdout=log,stderr=log,check=True)
 finally:
  server.terminate();server.wait(timeout=10)
print('Own Xvfb window present/capture and windowed-input check passed; own server stopped')
