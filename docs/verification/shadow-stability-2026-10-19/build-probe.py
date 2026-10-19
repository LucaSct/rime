# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
import pathlib,json,shlex,subprocess,os
root=pathlib.Path(os.environ.get('RIME_REPO_ROOT', pathlib.Path(__file__).resolve().parents[3]));build=root/'build/release';out=pathlib.Path(__file__).parent
entries=json.loads((build/'compile_commands.json').read_text());e=next(e for e in entries if e['file'].endswith('/tests/render/shadow_test.cpp'));raw=shlex.split(e['command']);args=[raw[0]];i=1
while i<len(raw):
 if raw[i] in ['-o','-MF','-MT','-MQ']:i+=2;continue
 if raw[i] in ['-MD','-MMD']:i+=1;continue
 args.append(str(out/'probe-cascade.cpp') if raw[i]==e['file'] else raw[i]);i+=1
args+=['-o',str(out/'probe-cascade.o')];subprocess.run(args,cwd=build,check=True)
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','bin/rime_render_tests'],text=True).splitlines();raw=shlex.split(next(x for x in reversed(lines) if ' -o bin/rime_render_tests ' in x));start=next(i for i,x in enumerate(raw) if pathlib.Path(x).name in ['c++','g++','clang++']);stop=next((i for i in range(start+1,len(raw)) if raw[i]=='&&'),len(raw));args=raw[start:stop];args=[a for a in args if not a.endswith('.cpp.o')];args.insert(1,str(out/'probe-cascade.o'));override=os.environ.get('RIME_CASCADE_OBJECT');args.insert(1,override) if override else None;args[args.index('-o')+1]=str(out/('probe-cascade-baseline' if override else 'probe-cascade'));args=[('-Wl,--dependency-file='+str(out/'probe-link.d')) if a.startswith('-Wl,--dependency-file=') else a for a in args];subprocess.run(args,cwd=build,check=True)
