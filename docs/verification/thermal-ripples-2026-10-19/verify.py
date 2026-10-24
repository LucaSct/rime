import json, shlex, subprocess, pathlib, os
root=pathlib.Path('/home/next/projects/rime'); build=root/'build/release'
out=pathlib.Path(__file__).parent; db=json.loads((build/'compile_commands.json').read_text())
def flags(suffix, compiler, mode, dest=None):
    e=next(x for x in db if x['file'].endswith(suffix))
    raw=shlex.split(e['command']); result=[compiler]; i=1
    while i<len(raw):
        if raw[i] in ['-o','-MF','-MT']:
            i+=2; continue
        if raw[i] in ['-MD','-MMD']:
            i+=1; continue
        result.append('-O1' if raw[i]=='-O2' else raw[i]); i+=1
    if compiler=='clang++': result+=['-Wno-c2y-extensions','-Wno-#warnings']
    if mode=='syntax':
        result.remove('-c'); result+=['-fsyntax-only']
    else: result+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(dest)]
    return result
with (out/'clang-syntax.log').open('w') as log:
    for file in ['/engine/fluids_render/src/phase_detail.cpp','/engine/fluids_render/src/phase_surface.cpp','/tests/fluids_render/phase_surface_test.cpp','/samples/thermal-courtyard/scene.cpp','/samples/thermal-courtyard/main.cpp','/engine/app/editor_host_app.cpp','/engine/render/src/scene_renderer.cpp']:
        subprocess.run(flags(file,'clang++','syntax'),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Clang syntax: seven changed translation units passed',flush=True)
raw=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/rime_fluids_render_tests'],text=True)
link=shlex.split(raw.splitlines()[-1]); assert link[:2]==[':','&&'] and link[-2:]==['&&',':']; link=link[2:-2]
units=['/engine/fluids_render/src/phase_detail.cpp','/engine/fluids_render/src/phase_surface.cpp','/tests/fluids_render/phase_surface_test.cpp','/engine/render/src/scene_renderer.cpp','/engine/fluids_scene/src/preview.cpp','/engine/fluids/src/thermal_field.cpp','/engine/fluids/src/thermal_contact.cpp','/engine/fluids/src/phase_change.cpp']
objects=[]
with (out/'asan-build.log').open('w') as log:
    for file in units:
        dest=out/(pathlib.Path(file).stem+'-asan.o'); objects.append(str(dest))
        subprocess.run(flags(file,'c++','asan',dest),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
    result=[]
    for token in link:
        if token.startswith('tests/fluids_render/') and token.endswith('.o'): result+=objects
        else: result.append(token)
    result[result.index('-o')+1]=str(out/'asan-tests')
    result=[('-Wl,--dependency-file='+str(out/'asan.link.d')) if t.startswith('-Wl,--dependency-file=') else t for t in result]
    result+=['-fsanitize=address,undefined']; subprocess.run(result,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
os.environ['ASAN_OPTIONS']='detect_leaks=0' # GPU policy matches CI; separate CPU run retains LSan
with (out/'asan-gpu-tests.log').open('w') as log:
    r=subprocess.run([str(out/'asan-tests'),'--no-colors'],stdout=log,stderr=log)
print('ASan/UBSan exit',r.returncode, (out/'asan-gpu-tests.log').read_text()[-450:],flush=True); r.check_returncode()
