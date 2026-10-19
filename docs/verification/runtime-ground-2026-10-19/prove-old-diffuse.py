# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import pathlib,json,shlex,subprocess,os
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent/'old-diffuse';out.mkdir(exist_ok=True)
source=out/'pbr_forward_shadowed.frag';source.write_text(subprocess.check_output(['git','show','727c851:engine/render/shaders/pbr_forward_shadowed.frag'],cwd=root,text=True))
cache=(build/'CMakeCache.txt').read_text();compiler=next(x.split('=',1)[1] for x in cache.splitlines() if x.startswith('RIME_GLSL_COMPILER:'))
subprocess.run([compiler,'-V','--target-env','vulkan1.3','-I'+str(root/'engine/render/shaders'),'-o',str(out/'old.spv'),str(source)],check=True)
subprocess.run(['cmake','-DSPV='+str(out/'old.spv'),'-DHDR='+str(out/'pbr_forward_shadowed.frag.spv.h'),'-DSYM=pbr_forward_shadowed_frag_spv','-P',str(root/'cmake/embed_spirv.cmake')],check=True)
e=next(e for e in json.loads((build/'compile_commands.json').read_text()) if e['file'].endswith('/engine/render/src/passes.cpp'));raw=shlex.split(e['command']);args=[raw[0],'-I'+str(out)];i=1
while i<len(raw):
 if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
 if raw[i] in ['-MD','-MMD']:i+=1;continue
 args.append(raw[i]);i+=1
args+=['-o',str(out/'old-passes.o')];subprocess.run(args,cwd=build,check=True)
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/rime_render_tests'],text=True).splitlines();raw=shlex.split(next(x for x in reversed(lines) if ' -o bin/rime_render_tests ' in x));start=next(i for i,x in enumerate(raw) if pathlib.Path(x).name in ['c++','g++','clang++']);stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));args=raw[start:stop];args.insert(1,str(out/'old-passes.o'));exe=out/'old-diffuse-test';args[args.index('-o')+1]=str(exe);args=[('-Wl,--dependency-file='+str(out/'link.d')) if a.startswith('-Wl,--dependency-file=') else a for a in args];subprocess.run(args,cwd=build,check=True)
env=os.environ.copy();env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['RIME_REQUIRE_VULKAN']='1'
result=subprocess.run([str(exe),'--test-case=ddgi surface*'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(out.parent/'old-diffuse-control.log').write_text(result.stdout);assert result.returncode!=0,'original surface query unexpectedly passed new regression'
print('PASS sensitivity control: original production diffuse query fails current surface assertions; exit',result.returncode)
