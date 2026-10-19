# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import os,pathlib,json,shlex,subprocess
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent
entries=json.loads((build/'compile_commands.json').read_text());objects={}
def entry(source): return next(e for e in entries if e['file'].endswith('/'+source))
def compile(source,syntax=False):
 e=entry(source);raw=shlex.split(e['command']);args=['clang++' if syntax else raw[0]];i=1
 while i<len(raw):
  if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
  if raw[i] in ['-MD','-MMD']:i+=1;continue
  args.append(raw[i]);i+=1
 target=out/(source.replace('/','_')+'.asan.o')
 if syntax:args.remove('-c');args+=['-fsyntax-only','-Wno-c2y-extensions','-Wno-#warnings']
 else:args+=['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(target)]
 subprocess.run(args,cwd=build,check=True);return target
def obj(source):
 if source not in objects:objects[source]=compile(source)
 return objects[source]
def link(target,sources):
 lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/'+target],text=True).splitlines()
 raw=shlex.split(next(line for line in reversed(lines) if ' -o bin/'+target+' ' in line));start=next(i for i,t in enumerate(raw) if pathlib.Path(t).name in ['c++','g++','clang++']);stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));args=raw[start:stop]
 for source in sources:
  replacement=str(obj(source));matches=[i for i,a in enumerate(args) if a.endswith('/'+pathlib.Path(source).name+'.o')]
  assert len(matches)<=1,(source,matches)
  if matches:args[matches[0]]=replacement
  else:args.insert(1,replacement)
 result=out/(target+'-changed-tu-asan');args[args.index('-o')+1]=str(result)
 args=[('-Wl,--dependency-file='+str(result)+'.link.d') if a.startswith('-Wl,--dependency-file=') else a for a in args];args+=['-fsanitize=address,undefined'];subprocess.run(args,cwd=build,check=True);return result
sources=['engine/render/src/lighting/shadows.cpp','engine/render/src/passes.cpp','tests/render/shadow_stability_test.cpp','tests/render/ddgi_surface_query_test.cpp','samples/99-the-block/main.cpp']
for source in sources:
 compile(source,True);print('Clang syntax PASS',source,flush=True)
changed=[source for source in sources if source!='samples/99-the-block/main.cpp']
exe=link('rime_render_tests',changed)
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
subprocess.run([str(exe),'--test-case=shadow stability:*,shadow distance:*'],cwd=build,env=env,check=True)
print('Changed-TU CPU ASan/UBSan/LSan PASS',flush=True)
env['ASAN_OPTIONS']='detect_leaks=0';env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['RIME_REQUIRE_VULKAN']='1'
subprocess.run([str(exe),'--test-case=ddgi surface query:*'],cwd=build,env=env,check=True)
print('Changed-TU Vulkan ASan/UBSan PASS; GPU driver leak checks disabled',flush=True)
print('Dependency archives are not fully instrumented. No threading primitives changed.',flush=True)
