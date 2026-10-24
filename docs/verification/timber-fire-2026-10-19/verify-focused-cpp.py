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

for source in ['engine/blockkit/src/palette.cpp','tests/blockkit/blockkit_test.cpp','engine/vfx_scene/src/effects.cpp','tests/vfx_scene/effects_test.cpp']:
 compile(source,True);print('Clang syntax PASS',source,flush=True)
for target,tests,implementations in [('blockkit_test',['tests/blockkit/blockkit_test.cpp'],['engine/blockkit/src/palette.cpp']),('rime_vfx_scene_tests',['tests/vfx_scene/effects_test.cpp'],['engine/vfx_scene/src/effects.cpp'])]:
 exe=link(target,tests,implementations)
 env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
 subprocess.run([str(exe)],cwd=build,env=env,check=True)
 print('Changed-TU ASan/UBSan + LSan PASS',target,flush=True)
print('Dependency archives are not fully instrumented.',flush=True)
