import pathlib,json,shlex,subprocess,os
root=pathlib.Path('/home/next/projects/rime');build=root/'build/release';out=pathlib.Path(__file__).parent
db=json.loads((build/'compile_commands.json').read_text())
def entry(name,target):return next(e for e in db if e['file'].endswith('/samples/99-the-block/'+name) and '/'+target+'.dir/' in e['command'])
def command(e,syntax=False,dest=None):
 raw=shlex.split(e['command']);result=['clang++' if syntax else raw[0]];i=1
 while i<len(raw):
  if raw[i] in ['-o','-MF','-MT']:i+=2;continue
  if raw[i] in ['-MD','-MMD']:i+=1;continue
  result.append('-O1' if raw[i]=='-O2' else raw[i]);i+=1
 if syntax:result.remove('-c');result+=['-fsyntax-only']
 else:result+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-o',str(dest)]
 return result
units=[('main.cpp','the_block'),('block_host_main.cpp','the_block_host'),('play_city.cpp','the_block_host')]
with (out/'clang-syntax.log').open('w') as log:
 for name,target in units:subprocess.run(command(entry(name,target),True),cwd=build,stdout=log,stderr=log,check=True)
print('Clang: three sample/host TUs passed',flush=True)
objects={}
with (out/'asan-build.log').open('w') as log:
 for name,target in units:
  dest=out/(target+'-'+name+'.asan.o');objects[(name,target)]=str(dest)
  subprocess.run(command(entry(name,target),dest=dest),cwd=build,stdout=log,stderr=log,check=True)
 for target,binary in [('the_block','the_block'),('the_block_host','the-block-host')]:
  lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/'+binary],text=True).splitlines()
  raw=shlex.split(next(line for line in reversed(lines) if ' -o bin/'+binary+' ' in line))
  start=next(i for i,t in enumerate(raw) if pathlib.Path(t).name in ['c++','g++','clang++'])
  stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw))
  link=raw[start:stop]
  for (name,owned_target),dest in objects.items():
   if owned_target!=target:continue
   for i,t in enumerate(link):
    if t.endswith('/'+name+'.o') and '/'+target+'.dir/' in t:link[i]=dest
  result=out/(binary+'-asan');link[link.index('-o')+1]=str(result)
  link=[('-Wl,--dependency-file='+str(out/(binary+'-asan.link.d'))) if t.startswith('-Wl,--dependency-file=') else t for t in link]
  link+=['-fsanitize=address,undefined'];subprocess.run(link,cwd=build,stdout=log,stderr=log,check=True)
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
with (out/'asan-scene-info.log').open('w') as log:
 subprocess.run([str(out/'the_block-asan'),'--scene-info','--scene',str(out/'city-demo.rscene'),'--content','/home/next/rime-exports/city-dev-test-2026-10-19/content'],env=env,stdout=log,stderr=log,check=True)
print('ASan/UBSan CPU city load passed, leak detection enabled',flush=True)
