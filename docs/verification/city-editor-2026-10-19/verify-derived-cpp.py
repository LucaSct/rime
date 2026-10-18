import pathlib,json,shlex,subprocess,os
root=pathlib.Path('/home/next/projects/rime');build=root/'build/release';out=pathlib.Path(__file__).parent
db=json.loads((build/'compile_commands.json').read_text())
def entry(suffix):return next(e for e in db if e['file'].endswith(suffix))
def compile(e,syntax,output):
 raw=shlex.split(e['command']);result=['clang++' if syntax else raw[0]];i=1
 while i<len(raw):
  if raw[i] in ['-o','-MF','-MT']:i+=2;continue
  if raw[i] in ['-MD','-MMD']:i+=1;continue
  result.append('-O1' if raw[i]=='-O2' else raw[i]);i+=1
 if syntax:
  result.remove('-c');result+=['-fsyntax-only','-Wno-c2y-extensions','-Wno-#warnings']
 else:result+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(output)]
 subprocess.run(result,cwd=build,check=True)
units=['engine/blockkit/src/palette.cpp','tests/blockkit/blockkit_test.cpp','samples/thermal-courtyard/scene.cpp']
for name in units:compile(entry(name),True,None)
for name in units[:2]:compile(entry(name),False,out/(pathlib.Path(name).name+'.asan.o'))
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/blockkit_test'],text=True).splitlines()
raw=shlex.split(next(line for line in reversed(lines) if ' -o bin/blockkit_test ' in line));start=next(i for i,t in enumerate(raw) if pathlib.Path(t).name in ['c++','g++','clang++']);stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));link=raw[start:stop]
link=[str(out/'blockkit_test.cpp.asan.o') if t.endswith('/blockkit_test.cpp.o') else t for t in link]
link[link.index('-o')+1]=str(out/'blockkit-derived-asan')
link=[('-Wl,--dependency-file='+str(out/'blockkit-derived-asan.link.d')) if t.startswith('-Wl,--dependency-file=') else t for t in link]
link.insert(next(i for i,t in enumerate(link) if t=='lib/librime_blockkit.a'),str(out/'palette.cpp.asan.o'))
link+=['-fsanitize=address,undefined'];subprocess.run(link,cwd=build,check=True)
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
subprocess.run([str(out/'blockkit-derived-asan')],cwd=build,env=env,check=True)
print('Clang: palette/test/courtyard passed; ASan/UBSan palette and blockkit regression passed with leak detection')
