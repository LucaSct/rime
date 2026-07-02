# The provenance panel: showing *why* a computed part is what it is (viewer E3)

The viewer can already show ICEM's computed **geometry** (an STL), its computed **fields** (an `.icef`
coloured/sliced/raymarched), and now its computed **reasoning**. ICEM is a *deterministic* engineering
model: every number it emits — a wall thickness, a stress, a safety margin — is produced by a named law,
material property, rule, or safety factor. ICEM records that chain as a **Ledger**, and the viewer renders
it as a scrollable **provenance panel** on the from-scratch UI ([ADR-0015](../adr/0015-imgui-free-ui.md),
[ui-text-layout](ui-text-layout.md)). Selecting a value shows *why it is what it is*: the values it was
computed from. Code: `provenance.hpp` (reader), `provenance_view.hpp` (layout), `main.cpp`
(`run_provenance_*`); proof: `tests/rhi/provenance_offscreen_test.cpp`.

This is the third leg of the **cross-repo viewer initiative**. The two repos share **only a file format**,
never code; the ICEM side that writes it is documented in the icem repo's `docs/math/provenance-io.md`.

## The Ledger is a DAG

Every value ICEM computes is a node

$$ n = \big(\,\text{id},\ \text{origin},\ \text{label},\ \text{value (SI)},\ \text{unit},\ \text{inputs}[\,]\,\big), $$

where `inputs` holds the **ids of the nodes that caused this value**. Because a node can only be built
from nodes that already exist, every edge points from a higher id to a lower one: the ledger is a
**directed acyclic graph (DAG)**, topologically sorted by construction. Its sources are the given
`Input`s, the `Material` properties and the `SafetyFactor`s; its sinks are the `Rule` checks and the final
`Derived` verdict. The `origin` is the vocabulary of the *logic of engineering* — what kind of knowledge
produced the number:

| origin | tag | colour | meaning |
|---|---|---|---|
| `Input` | INP | blue | a specification given |
| `Law` | LAW | amber | a physics equation |
| `Material` | MAT | teal | a material property |
| `Rule` | CHK | green | a design check (margin $\ge 1$) |
| `Heuristic` | HEU | purple | encoded practical experience |
| `SimResult` | SIM | orange | a simulation result |
| `SafetyFactor` | SF | red | a margin deliberately applied |
| `Derived` | DRV | neutral | an intermediate / the verdict |

Reading the colours top-to-bottom is reading the design itself: blue givens and teal/red material+margins
flow through amber laws and purple experience into green checks and the verdict.

## `.icejson`: parseable without a JSON library

ICEM serialises the ledger as a small UTF-8 document written **one node object per line** inside a
one-line header object:

```
{"kind":"icem-provenance","version":1,"design":"<name>","hash":"<16-hex>","passes":<bool>,"nodes":[
{"id":0,"origin":"Input","label":"chamber pressure","value":6000000,"unit":"Pa","inputs":[]},
...
{"id":24,"origin":"Derived","label":"verdict: ...","value":1,"unit":"bool","inputs":[18,22,23]}
]}
```

The line discipline is the whole trick: the reader (`load_provenance`) never needs a JSON parser. It scans
line by line — the one containing `"kind":"icem-provenance"` is the header (it lifts `design`, `hash`,
`passes` from it); each line starting with `{"id":` is a node, pulled apart by three tiny field scans
(`"key":"…"` for strings, `"key":<num>` for numbers, `"inputs":[…]` for the id array). Node `label`s are
plain engineering text with nothing to escape. A file missing the header, or with no nodes, is rejected
(`std::nullopt`) rather than misparsed — so pointing `--provenance` at the wrong file fails cleanly.

The carried `hash` is the **ledger's own content hash** (ICEM's determinism gate): the same design hashes
identically across runs and platforms, and `.icejson` is a pure function of the ledger, so it round-trips
bit-for-bit. The viewer shows the hash in the header band as the provenance's fingerprint.

## Derivation on demand: walking one level of the DAG

The panel is a vertical list, one row per node, drawn in id order: an origin tag, then
`label = value unit` (value formatted `%.4g` — enough to read, not the storage precision). Clicking a row
toggles it **selected**; a selected node expands its **derivation** inline — for each id $c$ in its
`inputs`, a sub-row `← label_c = value_c unit_c`. That is exactly one backward step along the DAG: *this
value, because of these values*. A source node (no inputs) expands to `← given (a specification input)`,
the place the chain bottoms out. Showing one level keeps the panel readable; the reader can walk further
by selecting a cause in turn.

## Layout, scrolling, and cheap clipping

Geometry is in screen pixels (see [ui-text-layout](ui-text-layout.md)). With body text scale $s$ and the
font cell height $H_g$, each row is $r = H_g s + 8$ px tall; a fixed header band of $h = 86$ px carries the
title, the PASS/FAIL verdict, the hash and the hint. The list starts at $y_0 = h$ and is shifted up by a
scroll offset $\sigma$ (mouse wheel), so node $i$'s top — accounting for the sub-rows of any expanded
earlier nodes — is laid out by a running pen $y$ that advances $r$ per row and $0.85\,r$ per sub-row. The
function returns the total **content height** $C$ (the sum of all advances), and the caller clamps

$$ \sigma \in \big[\,0,\ \max(0,\ C - (H - h))\,\big], $$

so you can scroll exactly until the last row reaches the bottom and no further. The header band is drawn
**after** the rows (last write wins, the UI being painter-ordered and opaque), so rows scrolled up vanish
cleanly behind it without a scissor rectangle. Each row is emitted only while on-screen
($y + r > h$ and $y < H$) — clipping by *skipping work*, not by masking pixels, which keeps a thousand-node
ledger cheap. Hit-testing reuses the UI's `hovered_in` / `clicked_in` against each row rect, so the same
rows that draw are the ones that select.

## What the test pins

`provenance_offscreen_test.cpp` proves the contract in three parts, the first two GPU-free so they always
run in CI: (1) the **reader** lifts the header and every node out of a sample `.icejson` and rejects a
non-provenance file; (2) the **layout** selects a row on click, clears it on a second click, and grows the
content height when a node with causes is expanded; (3) the **render** (off-screen, skipped without a
device) draws the panel — fill covers the frame, bright bitmap-font text appears, and the green of the
PASS verdict and the CHK tag shows.
