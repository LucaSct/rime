from pathlib import Path
import subprocess,difflib,json
out=Path(__file__).parent;root=Path('/home/next/projects/rime');build=root/'build/release'
# Reuse flag reconstruction only; never rerun the sanitizer/build body by importing it.
namespace={};exec((out/'sanitize.py').read_text().split('units=')[0].replace('out=pathlib.Path(__file__).parent','out=pathlib.Path('+repr(str(out))+')'),namespace)
flags=namespace['flags']
def plain_flags(suffix,dest,source=None):
 a=flags(suffix,'c++','asan',dest,source);i=a.index('-fsanitize=address,undefined');return a[:i]+['-o',str(dest)]
phase=out/'phase-gcc.o'
with (out/'mutant-build.log').open('w') as log:
 subprocess.run(plain_flags('/engine/fluids/src/phase_change.cpp',phase),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 original=(root/'engine/fluids/src/thermal_contact.cpp').read_text()
 changes={'partial-commit':('cold = next_cold;','// Deliberately missing second commit.'),
          'lost-residual':('result.second = first_hot ? cold_receipt : hot_receipt;',
                           'result.second = first_hot ? cold_receipt : hot_receipt;\n            result.second.roundoff_joules = 0.0;')}
 for name,(old,new) in changes.items():
  assert original.count(old)==1
  directory=out/name;directory.mkdir(exist_ok=True)
  mutated=original.replace(old,new);source=directory/'thermal_contact.cpp';source.write_text(mutated)
  (directory/'mutation.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),mutated.splitlines(True),fromfile='original',tofile=name,n=0)))
  obj=directory/'contact.o';subprocess.run(plain_flags('/engine/fluids/src/thermal_contact.cpp',obj,source),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
  archive=directory/'libmutant.a';subprocess.run(['ar','rcs',str(archive),str(obj),str(phase)],check=True)
  command=['python3',str(root/'tests/fluids/verify_thermal_contact.py'),'--library',str(archive),'--work-dir',str(directory/'check')]
  r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  (directory/'checker.log').write_text(r.stdout)
  assert r.returncode!=0 and 'AssertionError' in r.stdout and not (directory/'check/thermal-contact-report.json').exists()
  print(name,'correctly rejected',r.returncode,r.stdout.splitlines()[-1],flush=True)
