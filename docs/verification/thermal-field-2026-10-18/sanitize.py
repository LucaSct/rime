import pathlib,json,shlex,subprocess
root=pathlib.Path('/home/next/projects/rime'); build=root/'build/release'; out=pathlib.Path(__file__).parent
entries=json.loads((build/'compile_commands.json').read_text())
def flags(suffix,compiler,mode,dest=None,source=None):
 e=next(v for v in entries if v['file'].endswith(suffix));raw=shlex.split(e['command']);a=[compiler];i=1
 while i<len(raw):
  if raw[i] in ['-o','-MF','-MT']:i+=2;continue
  if raw[i] in ['-MD','-MMD']:i+=1;continue
  token=raw[i]
  if source and token==e['file']:token=str(source)
  a.append('-O1' if token=='-O2' else token);i+=1
 if compiler=='clang++':a+=['-Wno-c2y-extensions','-Wno-#warnings']
 if mode=='syntax':a.remove('-c');a+=['-fsyntax-only']
 else:a+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(dest)]
 return a
units=['/engine/fluids/src/thermal_field.cpp','/tests/fluids/thermal_field_test.cpp']
with (out/'clang-syntax.log').open('w') as log:
 for file in units:subprocess.run(flags(file,'clang++','syntax'),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Both new C++ translation units pass Clang syntax checks',flush=True)
wrapper=out/'thermal-field-tests.cpp';wrapper.write_text('#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include '+json.dumps(str(root/'tests/fluids/thermal_field_test.cpp'))+'\n')
with (out/'asan-build.log').open('w') as log:
 objects=[]
 for suffix,source,name in [(units[0],None,'field'),('/engine/fluids/src/thermal_contact.cpp',None,'contact'),('/engine/fluids/src/phase_change.cpp',None,'phase'),(units[1],wrapper,'tests')]:
  obj=out/(name+'-asan.o');objects.append(obj);subprocess.run(flags(suffix,'c++','asan',obj,source),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 binary=out/'asan-tests'
 subprocess.run(['c++',*[str(p) for p in objects],'-fsanitize=address,undefined','-o',str(binary)],stdout=log,stderr=subprocess.STDOUT,check=True)
with (out/'asan-tests.log').open('w') as log:r=subprocess.run([str(binary),'--no-colors'],stdout=log,stderr=log)
print('ASan/UBSan',r.returncode,(out/'asan-tests.log').read_text()[-500:],flush=True);r.check_returncode()
with (out/'clang-archive-build.log').open('w') as log:
 objects=[]
 for suffix,name in [(units[0],'field'),('/engine/fluids/src/thermal_contact.cpp','contact'),('/engine/fluids/src/phase_change.cpp','phase')]:
  obj=out/(name+'-clang.o');objects.append(obj)
  a=flags(suffix,'clang++','asan',obj);i=a.index('-fsanitize=address,undefined');a=a[:i]+['-o',str(obj)]
  subprocess.run(a,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run(['ar','rcs',str(out/'libthermal-clang.a'),*[str(p) for p in objects]],stdout=log,stderr=subprocess.STDOUT,check=True)
print('Clang runtime archive built',flush=True)
