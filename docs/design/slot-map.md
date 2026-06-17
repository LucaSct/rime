# Slot map & generational handles — design note (M1.5)

Companion to `engine/core/include/rime/core/containers/`. The math bricks ship *derivations*;
the systems bricks ship *design notes* like this one — the reasoning and trade-offs behind a
technique, so the repo stays legible enough to learn engine development from.

## The problem

Engine systems are full of long-lived references: an entity refers to its mesh, a material to
its textures, a system to the entities it touches. Two naive options both fail at scale:

- **Raw pointers / iterators into a `std::vector`.** Dangle the moment the vector grows and
  reallocates, or an element is removed. One realloc and every stored pointer is a landmine.
- **`std::unordered_map<Id, T>`.** Stable ids, but pointer-chasing nodes scattered across the
  heap — cache-hostile, and iterating "all live elements" (every frame, for every system) is
  slow. Data-oriented engines live or die on that iteration.

We want all of: **stable references**, **O(1)** insert/erase/lookup, and **contiguous iteration**.

## The idea: a generational handle + a dense/sparse split

A **handle** is not a pointer; it is `{ index, generation }`, two 32-bit integers
(`containers/handle.hpp`). It names a *slot*, not a memory address. Storage is split in two
(`containers/slot_map.hpp`):

```
handle.index ─▶ slots_[index] = { generation, dense_index }
                                              │
                                              ▼
                              dense_  = [ v0 v1 v2 v3 … ]   (packed, no gaps)
                              dense_to_slot_ = [ s0 s1 s2 … ] (parallel back-pointers)
```

- `dense_` holds the values **packed with no gaps**, so iterating the live set is a flat linear
  scan — the cache-friendly hot path.
- `slots_` is the **sparse** table indexed by `handle.index`. Each slot stores a `generation`
  stamp and the position of its value within `dense_`. This indirection is what lets the dense
  array move values around (on erase) without invalidating handles.
- `dense_to_slot_` runs parallel to `dense_`: for each packed value, which slot owns it. Needed
  to repoint a slot when its value is relocated.

### Lookup — `get` / `contains`

`handle.index → slots_[index].dense_index → dense_[…]`, but only after the slot's `generation`
matches the handle's. The generation check is the whole safety story (below). O(1), two indirections.

### Insert

Take a slot from the free list (or grow `slots_` by one), point it at the end of `dense_`, and
`push_back` the value. O(1) amortized. Returns `{ slot_index, slot.generation }`.

### Erase — swap-and-pop

To keep `dense_` gap-free, move the **last** dense element into the hole, fix up *its* owning
slot's `dense_index` (via `dense_to_slot_`), then pop the tail. Mark the freed slot
`dense_index = kInvalidSlotIndex`, **bump its generation**, and push it on the free list. O(1),
no shifting. (Order is not preserved — fine for an unordered store.)

## Why the generation defeats use-after-free

This is the property worth dwelling on. Suppose a slot is allocated at generation `g`, handing
out handle `H = {i, g}`. Later the element is erased: the slot's generation becomes `g+1`. Now:

- **Stale lookup.** `contains(H)` compares `slots_[i].generation` (`= g+1`) against `H.generation`
  (`= g)` → mismatch → rejected. The dangling handle reads *nothing*, instead of silently
  aliasing whatever now lives there.
- **Slot reuse.** When the slot is recycled, the new occupant gets handle `{i, g+1}`. The old
  `H = {i, g}` still mismatches. So two different lifetimes that happen to share a physical slot
  are distinguished by the generation — exactly the bug raw indices can't catch.

A freed slot's bumped generation is never equal to any *outstanding* handle's, so a generation
match is sufficient evidence the element is live and the same one the handle was issued for.
(`contains` also checks the slot is currently occupied and the index is in range, so fabricated
or out-of-range handles are rejected gracefully rather than crashing — see the tests.)

**Caveat — wraparound.** `generation` is 32-bit; after `2^32` reuses of a *single* slot it wraps
and a very old handle could alias again. At thousands of reuses/sec that is years, and we can
widen or steal a bit if a system ever needs it. Noted, not yet a problem.

## Complexity & cost

| Operation        | Cost            | Notes                                             |
|------------------|-----------------|---------------------------------------------------|
| `insert`/`emplace` | O(1) amortized | reuses a free slot (LIFO, cache-warm) or grows    |
| `get`/`contains` | O(1)            | two indirections + a generation compare           |
| `erase`          | O(1)            | swap-and-pop; no element shifting                 |
| iterate live set | O(n), contiguous| flat scan over `dense_` — the per-frame hot path  |
| handle size      | 8 bytes         | trivially copyable; pass by value everywhere      |

## Deliberate limitations (labeled, per CLAUDE.md)

- **Backing storage is `std::vector`.** Correct and clear, but it allocates from the default
  heap. Drawing from the engine's allocators (M1.2) is a planned seam, not done here — the
  algorithm is the lesson for this brick.
- **Unordered.** Swap-and-pop does not preserve insertion order. Systems that need ordering sort
  on iteration or use a different structure.
- **Not thread-safe.** External synchronization is required for concurrent mutation; the job
  system (M1.6) defines how shared state is accessed.

## Where this goes

The slot map is the storage primitive the **ECS** (M4) builds component pools on, and that the
**asset** and **GPU-object** systems use for handle-based references. Phantom-typing the handle
(`Handle<T>`) means `Handle<Mesh>` and `Handle<Texture>` are distinct types — the compiler stops
you from crossing the streams, at zero runtime cost. *Inspired by: EnTT / Bevy slot maps and the
generational-index pattern.*
