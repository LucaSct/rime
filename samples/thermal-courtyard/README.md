# Frost Yard — a working thermal city courtyard

From the repository root:

```sh
./scripts/thermal-courtyard.sh
```

The script builds the sample and opens the saved scene. **Space / P** pauses or
resumes, **R** restarts, **Esc** closes. The fixed overview keeps all three
stations visible, including in a narrow tiled window. The demo pauses itself at
36 simulated seconds, after completing both phase changes; press R to replay.

![The completed courtyard demo on RTX 3060](../../docs/verification/thermal-courtyard-2026-10-18/yard-native.png)

| Station, left to right | What happens |
| --- | --- |
| Cyan panel, one pip — cooling | 10 kg of liquid water starts at 0 C; explicit -100 kW removes sensible/latent energy. Moving ripples settle into blue frost veins as it freezes, then cools below 0 C. |
| Amber panel, two pips — melting | 10 kg of ice starts at 0 C; explicit +100 kW melts it, then warms the liquid. It turns blue/smooth and develops ripples. |
| Dark panel, three pips — control | The same initial water with no heat source stays unchanged. |

At 36 seconds the measured states are **260.236 K, fully frozen**;
**279.491 K, fully liquid**; and **273.150 K, unchanged liquid**. During each
latent plateau temperature stays at 273.15 K while the liquid mass fraction
changes. The overlay reads numerical state, not a timer-driven colour animation.

The heater brazier and chimney use existing authored fire/smoke sources. Fire
provides cosmetic light/smoke; the +100 kW is a separate explicit thermal source.
Turning a cosmetic fire off does not implicitly extinguish the thermal source.
This small city diorama uses built-in meshes/materials and needs no downloads,
asset cook or external texture. Source/palette roles reconstruct draw data when
loading its `.rscene`; generated runtime registry IDs are not baked into it.

## Open and edit the same scene

```sh
./scripts/thermal-courtyard.sh --editor
```

See the [creator workflow and controls](../../docs/creator-workflow.md).
**Hold right mouse while pressing WASD** to move; Q/E down/up, Shift faster,
right-drag to look. Release right mouse for W/E/R object tools. Wheel over the
viewport moves closer/farther. The editor stream remains 960×540, so a large
panel can look soft; camera dolly is not image enlargement.

Open View → Outliner to select meshless sources. Filter → Effects (smoke / fire)
finds emitters, Filter → Thermal (water / ice) finds thermal descriptions, and
All entities restores the list. Display → List / Tiles switches between
small role icons beside names and a compact 32-pixel icon grid. Select an emitter, then edit its LocalTransform
or use the Move gizmo. To create one: + spawn, select the new entity, Inspector →
+ add component → SmokeEmitter/FireEmitter, then enabled. View → Assets is the
cooked asset browser; this built-in demo has no manifest, so it is empty. A prefab
library and clickable emitter icons are future work.

Press Play or Step. Click a tray to inspect its `ThermalCell`, opt-in
`ThermalSurface` and read-only thermal preview values. Edit initial mass,
temperature and liquid fraction; source entities hold signed `power_watts` and a
reference to their tray. Relevant cell/contact/source edits restart the whole
thermal graph from initial conditions, including on Pause. Appearance enable
changes only the visual binding. Stop restores the authored scene. Use Save As
for an experiment, then reopen it with the same courtyard host.

Use this sample's host (`build/release/bin/thermal-courtyard`) when opening its
scene. Generic `rime-engine` does not know the sample-owned `courtyard::Part` and
`Station` content components. The launcher supplies the correct host automatically.
The standalone wrapper runs the authored scene for 36 seconds; editor Play is
user-controlled, so Pause/Stop before driving these constant sources beyond the
material range. Range refusal is explicit and retains the last committed state.

## Repeatable captures and checks

```sh
./scripts/thermal-courtyard.sh --headless --ticks 0 --capture /path/to/start.ppm
./scripts/thermal-courtyard.sh --headless --ticks 1800 --capture /path/to/30-seconds.ppm
./scripts/thermal-courtyard.sh --headless --ticks 2160 --capture /path/to/complete.ppm
build/release/bin/thermal-courtyard --selftest --scene samples/thermal-courtyard/courtyard.rscene
build/release/bin/thermal-courtyard --controls-selftest
```

For a GPU-less Linux machine, prefix captures with
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1`
(the installed Mesa path can differ). Headless mode advances fixed CPU ticks
then renders three identical-time presentation frames; it does not wait 36 real
seconds. PPM captures use the actual framebuffer size when windowed. A missing
display returns an error for the interactive mode; use headless capture instead.
The scene generator is `thermal-courtyard --write-demo file.rscene`.

Real-editor scripted proof, after building the editor:

```sh
tools/target/debug/editor --smoke --frames 3 --courtyard-preview --engine "$PWD/build/release/bin/thermal-courtyard" --scene "$PWD/samples/thermal-courtyard/courtyard.rscene"
```

See [verification and retained captures](../../docs/verification/thermal-courtyard-2026-10-18/README.md)
for save/reopen, real Step/Pause/Stop/new Play, pixel controls, sanitizers, SDK and
native/windowed results. Source is deliberately split into persistent scene
content (`scene.cpp`) and a small standalone/editor composition (`main.cpp`).

This is an opaque **phase presentation on fixed meshes**, not flowing water,
transparent/refraction optics, geometric ice thickness, expansion, collision or
breakable sheets. The colour/roughness blend visualizes liquid **mass** fraction;
immutable mipmapped normal/tint maps add cosmetic ripples and frost veins.
Pause/refusal hold their committed clock; rebuild restarts it.
It does not infer solid volume. There is no fuel/spread/damage authority or
multiplayer shipment proof. M18/performance and inherited city/block gates remain
open; this is a usable demo checkpoint, with no merge or production claim.
