# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,json,shlex,subprocess
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent
entries=json.loads((build/'compile_commands.json').read_text())
def entry(suffix):return next(e for e in entries if e['file'].endswith(suffix))
def compile(source,syntax=False):
 e=entry(source);raw=shlex.split(e['command']);args=['clang++' if syntax else raw[0]];i=1
 while i<len(raw):
  if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
  if raw[i] in ['-MD','-MMD']:i+=1;continue
  args.append(raw[i]);i+=1
 target=out/(source.replace('/','_')+'.asan.o')
 if syntax:
  args.remove('-c');args+=['-fsyntax-only','-Wno-c2y-extensions','-Wno-#warnings']
 else:args+=['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(target)]
 subprocess.run(args,cwd=build,check=True)
 return target
units=['engine/fluids_render/src/phase_maps.cpp','tests/fluids_render/phase_maps_test.cpp','samples/99-the-block/authored_environment.cpp','tests/gameplay/authored_environment_test.cpp','tests/gameplay/player_spawn_test.cpp','tests/gameplay/input_map_test.cpp','samples/99-the-block/main.cpp','samples/99-the-block/block_host_main.cpp','engine/vfx_scene/src/effects.cpp','tests/vfx_scene/effects_test.cpp','engine/render/src/fx_pass.cpp','engine/render/src/scene_renderer.cpp','tests/render/fx_particle_test.cpp','engine/app/editor_host_app.cpp','samples/thermal-courtyard/main.cpp']
for source in units:
 compile(source,True);print('Clang syntax PASS',source,flush=True)
compiled={}
def object(source):
 if source not in compiled:compiled[source]=compile(source)
 return compiled[source]
def link(target,tests,library_sources):
 lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/'+target],text=True).splitlines()
 raw=shlex.split(next(line for line in reversed(lines) if ' -o bin/'+target+' ' in line))
 start=next(i for i,t in enumerate(raw) if pathlib.Path(t).name in ['c++','g++','clang++'])
 stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));args=raw[start:stop]
 for source in tests:
  replacement=str(object(source));matches=[i for i,a in enumerate(args) if a.endswith('/'+pathlib.Path(source).name+'.o')]
  assert len(matches)==1,(source,matches);args[matches[0]]=replacement
 for source in library_sources:args.insert(1,str(object(source)))
 result=out/(target+'-changed-tu-asan');args[args.index('-o')+1]=str(result)
 args=[('-Wl,--dependency-file='+str(result)+'.link.d') if a.startswith('-Wl,--dependency-file=') else a for a in args]
 args+=['-fsanitize=address,undefined'];subprocess.run(args,cwd=build,check=True);return result
vfx=link('rime_vfx_scene_tests',['tests/vfx_scene/effects_test.cpp'],['engine/vfx_scene/src/effects.cpp'])
phase=link('rime_fluids_render_tests',['tests/fluids_render/phase_maps_test.cpp'],['engine/fluids_render/src/phase_maps.cpp'])
game=link('rime_gameplay_tests',['tests/gameplay/authored_environment_test.cpp','tests/gameplay/player_spawn_test.cpp','tests/gameplay/input_map_test.cpp','samples/99-the-block/authored_environment.cpp'],['engine/vfx_scene/src/effects.cpp'])
render=link('rime_render_tests',['tests/render/fx_particle_test.cpp'],['engine/render/src/fx_pass.cpp','engine/render/src/scene_renderer.cpp'])
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1';env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['RIME_REQUIRE_VULKAN']='1'
for exe,pattern in [(vfx,None),(phase,'phase maps:*'),(game,'*marker*,*spawn*,city editor:*')]:
 command=[str(exe)]+(['--test-case='+pattern] if pattern else [])
 subprocess.run(command,env=env,cwd=build,check=True);print('ASan/UBSan + LSan PASS',exe.name,pattern,flush=True)
env['ASAN_OPTIONS']='detect_leaks=0'
for exe,pattern in [(game,'city authored environment:*'),(render,'*fx1a*,*smoke*,*flame V2*,*transient scene lights*,*SceneRenderer flame V2*')]:
 subprocess.run([str(exe),'--test-case='+pattern],env=env,cwd=build,check=True);print('ASan/UBSan GPU PASS (driver LSan off)',exe.name,pattern,flush=True)
print('Changed-TU instrumentation only; engine dependency archives were not rebuilt with sanitizers.',flush=True)
