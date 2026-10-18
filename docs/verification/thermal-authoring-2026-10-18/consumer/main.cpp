#include <rime/fluids_scene/preview.hpp>
#include <rime/editorhost/thermal_preview.hpp>
int main() {
 rime::ecs::World world; rime::fluids_scene::register_fluids_scene_components(world);
 rime::fluids_scene::ThermalCell c; c.enabled=true; c.liquid_mass_fraction=0;
 auto e=world.spawn_with(c); (void)world.spawn_with(rime::fluids_scene::ThermalSource{e,10000,true});
 rime::fluids_scene::ThermalPreview preview;
 if(!preview.tick(world,1,1./60.) || !preview.field() || preview.field()->cells()[0].state().liquid_mass_fraction<=0) return 1;
 rime::editorhost::ThermalPreviewMsg m; auto bytes=rime::editorhost::serialize_thermal_preview(m);
 rime::editorhost::ThermalPreviewMsg decoded; return !rime::editorhost::parse_thermal_preview(bytes,decoded);
}
