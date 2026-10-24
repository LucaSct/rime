# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import pathlib,json,shlex,subprocess,os
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent
revision=os.environ.get('RIME_SHADOW_SAMPLER_REVISION','38f44c3')
try:
 old=subprocess.check_output(['git','show',revision+':engine/render/shaders/sun_shadow.glsl'],cwd=root,text=True,stderr=subprocess.DEVNULL)
except subprocess.CalledProcessError:
 source=subprocess.check_output(['git','show',revision+':engine/render/shaders/pbr_forward_shadowed.frag'],cwd=root,text=True)
 a=source.index('float sun_shadow(');b=source.index('// A spot light\'s shadow factor',a);old=source[a:b]
(out/'old-sun-shadow.glsl').write_text(old)
s=(root/'tests/render/shaders/shadow_transition_probe.frag').read_text().replace('../../../engine/render/shaders/sun_shadow.glsl','old-sun-shadow.glsl');(out/'shadow_transition_probe.frag').write_text(s)
cache=(build/'CMakeCache.txt').read_text();compiler=next(x.split('=',1)[1] for x in cache.splitlines() if x.startswith('RIME_GLSL_COMPILER:'))
subprocess.run([compiler,'-V','--target-env','vulkan1.3','-I'+str(out),'-o',str(out/'old-sampler.spv'),str(out/'shadow_transition_probe.frag')],check=True)
subprocess.run(['cmake','-DSPV='+str(out/'old-sampler.spv'),'-DHDR='+str(out/'shadow_transition_probe.frag.spv.h'),'-DSYM=shadow_transition_probe_frag_spv','-P',str(root/'cmake/embed_spirv.cmake')],check=True)
e=next(e for e in json.loads((build/'compile_commands.json').read_text()) if e['file'].endswith('/shadow_transition_test.cpp'));raw=shlex.split(e['command']);args=[raw[0],'-I'+str(out)];i=1
while i<len(raw):
 if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
 if raw[i] in ['-MD','-MMD']:i+=1;continue
 args.append(raw[i]);i+=1
args+=['-o',str(out/'old-sampler-test.o')];subprocess.run(args,cwd=build,check=True)
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/rime_render_tests'],text=True).splitlines();raw=shlex.split(next(x for x in reversed(lines) if ' -o bin/rime_render_tests ' in x));start=next(i for i,x in enumerate(raw) if pathlib.Path(x).name in ['c++','g++','clang++']);stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));args=raw[start:stop];args=[a for a in args if not a.endswith('.cpp.o') or a.endswith('/render_graph_test.cpp.o')];args.insert(1,str(out/'old-sampler-test.o'));exe=out/'old-sampler-test';args[args.index('-o')+1]=str(exe);args=[('-Wl,--dependency-file='+str(out/'old-sampler-link.d')) if a.startswith('-Wl,--dependency-file=') else a for a in args];subprocess.run(args,cwd=build,check=True)
env=os.environ.copy();env['VK_DRIVER_FILES']='/usr/share/vulkan/icd.d/lvp_icd.json';env['RIME_REQUIRE_VULKAN']='1'
result=subprocess.run([str(exe),'--test-case=shadows: shared mesh*'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(out/'old-sampler-control.log').write_text(result.stdout);assert result.returncode!=0,'old hard cascade switch unexpectedly passed'
print('PASS sensitivity control: original sampler fails current continuity bounds; exit',result.returncode)
