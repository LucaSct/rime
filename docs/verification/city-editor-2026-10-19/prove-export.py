import pathlib,subprocess,sys,os,hashlib,shutil,json
from PIL import Image
root=pathlib.Path('/home/next/projects/rime');out=pathlib.Path(__file__).parent
sys.path.insert(0,str(root/'scripts'))
from game_bundle_manifest import verify,ManifestError
from export_city_dev import catalog,stage
bundle=pathlib.Path('/home/next/rime-exports/city-demo-2026-10-19')
digest='f58514e4655dd132da0f7369dadc4a5ea3c3a325dddde0297ef5deb5e13d7022'
verify(bundle,digest)
def run(args,expected=0,log=None):
 result=subprocess.run(args,text=True,capture_output=True,timeout=120)
 if log:(out/log).write_text(result.stdout+result.stderr)
 assert result.returncode==expected,(result.returncode,result.stdout,result.stderr)
 return result.stdout+result.stderr
isolation=['bwrap','--die-with-parent','--ro-bind','/usr','/usr','--ro-bind','/lib','/lib','--ro-bind','/lib64','/lib64','--ro-bind','/etc','/etc','--proc','/proc','--dev','/dev','--tmpfs','/tmp','--tmpfs','/home','--ro-bind',str(bundle),'/game','--chdir','/game']
text=run(isolation+['/game/the_block','--scene-info'],log='final-isolated-scene-info.log');assert '625 entities, 569 destructibles' in text
print('Final bundle integrity and strict scene/pattern binding passed with repository/home hidden',flush=True)
proof=out/'final-package-proof';proof.mkdir(exist_ok=True)
image=proof/'packaged-game.ppm'
if image.exists():image.unlink()
args=isolation+['--ro-bind','/tmp/.X11-unix','/tmp/.X11-unix','--bind',str(proof),'/proof','--unsetenv','WAYLAND_DISPLAY','--unsetenv','XAUTHORITY','--setenv','VK_DRIVER_FILES','/usr/share/vulkan/icd.d/lvp_icd.json','/game/the_block','--frames','4','--ppm','/proof/packaged-game.ppm']
text=run(['xvfb-run','-a','-s','-screen 0 1280x720x24 -ac',*args],log='final-isolated-game.log')
assert '4 frames, 24 ticks' in text
for weapon in ['held-scar-h.glb topology','held-m4a1.glb topology','held-msr.glb topology']:assert weapon in text
Image.open(image).save(out/'final-packaged-game.png')
print('Final copied binary defaults to Play: actual Vulkan window, 3 weapon assets, 4 frames/24 ticks, repository/home hidden',flush=True)
original=(bundle/'the_block').read_bytes()
try:stage(root/'build/release',bundle/'content/city.rscene',bundle)
except ValueError:pass
else:raise AssertionError('export replaced destination')
assert (bundle/'the_block').read_bytes()==original
print('Existing destination refused; executable unchanged',flush=True)
run([str(bundle/'the_block'),'--content',str(out/'missing-content'),'--scene-info'],expected=2,log='final-missing-explicit-content.log')
run(isolation+['--tmpfs','/game/content','/game/the_block','--scene-info'],expected=2,log='final-hidden-content.log')
print('Explicit wrong content and hidden bundle content both refused without checkout fallback',flush=True)
readme=bundle/'README.txt';original=readme.read_bytes()
try:
 readme.write_bytes(original+b'\ntampered\n')
 try:verify(bundle,digest)
 except (ValueError,ManifestError):pass
 else:raise AssertionError('changed payload accepted')
finally:readme.write_bytes(original)
fixture=out/'escaping-manifest';fixture.mkdir(exist_ok=True);(fixture/'manifest.txt').write_text('mesh\tmesh\t0000000000000001\t../escape.rmesh\n')
try:catalog(fixture)
except ValueError:pass
else:raise AssertionError('escaping cooked manifest accepted')
verify(bundle,digest)
print('Changed payload and escaping cooked target refused; restored final bundle verifies',flush=True)
