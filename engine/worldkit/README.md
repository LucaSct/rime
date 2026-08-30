# `rime::worldkit` — the component profile

One answer to *"what components does a Rime world have?"*, in one function, in one order.

```cpp
worldkit::register_engine_components(world);      // the engine's own
blockkit::register_blockkit_components(world);    // the game's, on top
```

Built at **m14.1** ([ADR-0037](../../docs/adr/0037-authoring-loop-m14.md)).

## Why it exists

There was no answer. Every consumer of a world hand-assembled its own registration list, and the
lists drifted — silently, because an unregistered component is not an error until something asks for
it. It cost two separate failures in one afternoon:

- **The editor could not open the block.** `engine/app/editor_host_main.cpp` registered transform +
  render + physics; `rime-blockgen` writes `blockkit::SlabRole`; the scene load failed on the newest
  content in the repo. The editor smoke stayed green throughout, because it used a synthetic scene.
- **`99-the-block` registered the four modules the block's *scene* needs and none of the four its
  *session* needs.** The block stood up and drew perfectly while the predictor never seeded (no
  `RigidBodyHandle` meant no body to predict against), destruction replicated zero ops, the peers'
  hashes disagreed, and the mixer heard nothing. Four symptoms, one missing list, and not one of them
  pointing at the cause.

**The tell, for next time:** a subsystem reporting *zero work* while everything around it looks
correct. Check the registration list before anything else.

## A profile, not a registry

The obvious fix — modules self-registering into a global table — was rejected in ADR-0037 on two
guardrails. It needs mutable global state (guardrail 4), and it turns *"the engine still builds
without this module"* into a runtime question rather than a link-time one (guardrail 2).

A profile is an ordinary function in an ordinary module that depends on everything it names. Delete
a feature module and this fails to **link**, loudly, at the one place that has to know.

## Two things worth knowing before changing it

**"Engine's" is load-bearing.** This names components the *engine* defines. It deliberately does not
name any game's or content module's — `blockkit` is the vision demo's content, and an engine profile
that knew about it would be an engine that cannot be used for a second game. Games layer their own on
top; that is one line and it is visible.

**Order is fixed and shared, and that is not tidiness.** `ecs::component_schema_hash` goes into
`net::NetDriver::Config`, and two peers that registered different sets refuse to connect. Both peers
calling this one function cannot disagree.

It also registers the two types no module's own `register_*_components` covers, because they are
derived state and so deliberately unreflected (`ecs/reflect.hpp`): `WorldTransform` (composed by
`propagate_transforms`; never rides a scene file, but the renderer, the picker and the gizmo pass all
query it live) and `RenderTransform` (the previous/current pair snapshot interpolation blends
between). The editor host and `99-the-block` had each rediscovered that separately, one line at a
time.

## Layering

The **top** of the cake. Depends on `ecs` (publicly — the one function takes an `ecs::World&`) plus
`physics`, `render`, `destruction`, `destruction_net`, `gameplay` and `gameplay_net` privately.
**Nothing depends on it**: samples, tools and the editor host call it; no engine module does. Delete
this directory and the engine still builds — the same promise `blockkit` and `destruction_render`
make.

Proofs: `tests/worldkit` — the profile registers every engine component type, is idempotent, gives
two independently-built worlds the same `component_schema_hash`, and (the case that earns the file)
loads the block's `.rscene` with **zero** unknown types.
