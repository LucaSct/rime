#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Build one external, count-only material-binding witness; never edit product files."""
import hashlib,json,pathlib,shlex,subprocess
root=pathlib.Path('/home/next/projects/rime')
build=root/'build/release'; out=pathlib.Path(__file__).resolve().parent
source=root/'engine/render/src/passes.cpp'; header=root/'engine/render/src/material_binding_cache.hpp'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
original=source.read_text(); generated=original
replacements=[('#include <iterator>','#include <iterator>\n#include <atomic>\n#include <cstdio>'),('    detail::MaterialBindingCache material_bindings;', '''    struct BindingProof {
        std::atomic<unsigned long long> invocations{0}, attempts{0}, issues{0}, draws{0};
        ~BindingProof() {
            std::fprintf(stderr, "RIME_BINDING_TOTAL invocations=%llu draws=%llu attempts=%llu issues=%llu\\n",
                invocations.load(), draws.load(), attempts.load(), issues.load());
        }
    };
    static BindingProof proof;
    unsigned long long binding_attempts = 0, binding_issues = 0;
    detail::MaterialBindingCache material_bindings;'''),('[&cmd](std::uint32_t binding, rhi::TextureHandle texture, rhi::SamplerHandle sampler) {','[&cmd, &binding_issues](std::uint32_t binding, rhi::TextureHandle texture, rhi::SamplerHandle sampler) {\n            ++binding_issues;'),('        if (bind_material_textures) {','        if (bind_material_textures) {\n            binding_attempts += 5;'),('        cmd.draw_indexed(item.index_count, 1, item.first_index);\n    }\n}', '''        cmd.draw_indexed(item.index_count, 1, item.first_index);
    }
    if (binding_attempts > 0) {
        const auto invocation = proof.invocations.fetch_add(1);
        proof.attempts.fetch_add(binding_attempts);
        proof.issues.fetch_add(binding_issues);
        proof.draws.fetch_add(binding_attempts / 5);
        if (invocation < 64)
            std::fprintf(stderr, "RIME_BINDING_ROW invocation=%llu draws=%llu attempts=%llu issues=%llu filter=%u match=%u caster=%u\\n",
                invocation, binding_attempts / 5, binding_attempts, binding_issues,
                flag_filter, flag_match, unsigned(caster_frustum.has_value()));
    }
}''')]
for needle,replacement in replacements:
 if generated.count(needle)!=1:raise RuntimeError(f'nonunique source anchor: {needle}')
 generated=generated.replace(needle,replacement)
copy=out/'passes-counted.cpp';copy.write_text(generated)
entries=json.loads((build/'compile_commands.json').read_text());entry=next(e for e in entries if pathlib.Path(e['file'])==source)
compile_cmd=shlex.split(entry['command']);compile_cmd[compile_cmd.index('-o')+1]=str(out/'passes-counted.o');compile_cmd[compile_cmd.index('-c')+1]=str(copy);compile_cmd.insert(1,'-I'+str(source.parent))
raw=subprocess.check_output(['ninja','-C',str(build),'-t','commands','the_block'],text=True).splitlines()[-1]
args=shlex.split(raw);args=args[args.index('/usr/bin/c++'):];args=args[:args.index('&&')]
args[args.index('-o')+1]=str(out/'the-block-binding-proof')
args=[('-Wl,--dependency-file='+str(out/'proof-link.d')) if x.startswith('-Wl,--dependency-file=') else x for x in args]
first_archive=next(i for i,a in enumerate(args) if a.endswith('.a'));args.insert(first_archive,str(out/'passes-counted.o'))
linked_files=[(build/a).resolve() if not pathlib.Path(a).is_absolute() else pathlib.Path(a) for a in args if a.endswith(('.a','.o'))]
provenance={'root':str(root),'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'product_source_sha256':{str(p):sha(p) for p in [source,header,root/'samples/99-the-block/city-v2.rscene']},'generated_source_sha256':sha(copy),'compile_command':compile_cmd,'link_command':args,'linked_input_sha256':{str(p):sha(p) for p in linked_files if p.exists()},'purpose':'count-only trace, not timing/FPS/baseline evidence'}
for name,cmd in [('compile.log',compile_cmd),('link.log',args)]:
 with (out/name).open('w') as f:subprocess.run(cmd,cwd=build,stdout=f,stderr=subprocess.STDOUT,check=True)
for p,expected in provenance['product_source_sha256'].items():
 if sha(pathlib.Path(p))!=expected:raise RuntimeError(f'product source changed: {p}')
provenance['object_sha256']=sha(out/'passes-counted.o');provenance['binary_sha256']=sha(out/'the-block-binding-proof')
(out/'build-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
print('External count-only binary ready; product source unchanged.')
