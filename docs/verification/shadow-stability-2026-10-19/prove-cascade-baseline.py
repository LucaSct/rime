# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import pathlib,subprocess,json,shlex,os,hashlib
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent
source=out/'baseline-shadows.cpp';source.write_bytes(subprocess.check_output(['git','show','38f44c3:engine/render/src/lighting/shadows.cpp'],cwd=root))
e=next(e for e in json.loads((build/'compile_commands.json').read_text()) if e['file'].endswith('/engine/render/src/lighting/shadows.cpp'));raw=shlex.split(e['command']);args=[raw[0]];i=1
while i<len(raw):
 if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
 if raw[i] in ['-MD','-MMD']:i+=1;continue
 args.append(str(source) if raw[i]==e['file'] else raw[i]);i+=1
obj=out/'baseline-shadows.o';args+=['-o',str(obj)];subprocess.run(args,cwd=build,check=True)
env=os.environ.copy();env['RIME_CASCADE_OBJECT']=str(obj);subprocess.run(['python',str(out/'build-probe.py')],env=env,check=True)
result=subprocess.run([str(out/'probe-cascade-baseline')],text=True,capture_output=True);(out/'cascade-before-reproduced.log').write_text(result.stdout+result.stderr);assert result.returncode==1,result.stdout
(out/'cascade-baseline-provenance.json').write_text(json.dumps({'source_commit':'38f44c3','source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'probe_sha256':hashlib.sha256((out/'probe-cascade.cpp').read_bytes()).hexdigest(),'baseline_binary_sha256':hashlib.sha256((out/'probe-cascade-baseline').read_bytes()).hexdigest(),'compile':args,'result':result.returncode,'note':'Original CSM object explicitly overrides current render archive; no stale archive dependency.'},indent=2)+'\n')
print(result.stdout.strip())
