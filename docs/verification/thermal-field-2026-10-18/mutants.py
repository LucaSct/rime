from pathlib import Path
import subprocess,difflib
out=Path(__file__).parent;root=Path('/home/next/projects/rime');build=root/'build/release'
namespace={};exec((out/'sanitize.py').read_text().split('units=')[0].replace('out=pathlib.Path(__file__).parent','out=pathlib.Path('+repr(str(out))+')'),namespace)
def compile_flags(suffix,dest,source=None):
 a=namespace['flags'](suffix,'c++','asan',dest,source);i=a.index('-fsanitize=address,undefined');return a[:i]+['-o',str(dest)]
with (out/'mutants-build.log').open('w') as log:
 objects=[]
 for suffix,name in [('/engine/fluids/src/phase_change.cpp','phase'),('/engine/fluids/src/thermal_contact.cpp','contact')]:
  obj=out/(name+'-plain.o');objects.append(obj);subprocess.run(compile_flags(suffix,obj),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 original=(root/'engine/fluids/src/thermal_field.cpp').read_text()
 assert original.count('result.heat_status = op.first.status;')==1
 partial=original.replace('result.heat_status = op.first.status;','result.heat_status = op.first.status;\n                cells_.swap(work_);')
 index=original.rindex('work_journal_.push_back(op);')
 lost=original[:index]+original[index:].replace('work_journal_.push_back(op);','// Deliberately omit a contact receipt.',1)
 allocated=original.replace('work_journal_.clear();','work_journal_.clear();\n    work_journal_.shrink_to_fit();')
 for name,mutated in [('partial-publication',partial),('lost-receipt',lost),('tick-allocation',allocated)]:
  directory=out/name;directory.mkdir(exist_ok=True);source=directory/'thermal_field.cpp';source.write_text(mutated)
  (directory/'mutation.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),mutated.splitlines(True),fromfile='original',tofile=name,n=0)))
  obj=directory/'field.o';subprocess.run(compile_flags('/engine/fluids/src/thermal_field.cpp',obj,source),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
  archive=directory/'libmutant.a';subprocess.run(['ar','rcs',str(archive),str(obj),*[str(o) for o in objects]],check=True)
  r=subprocess.run(['python3',str(root/'tests/fluids/verify_thermal_field.py'),'--library',str(archive),'--work-dir',str(directory/'check')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  (directory/'checker.log').write_text(r.stdout)
  assert r.returncode!=0 and 'AssertionError' in r.stdout and not (directory/'check/field-report.json').exists()
  print(name,'rejected',r.returncode,r.stdout.splitlines()[-1],flush=True)
