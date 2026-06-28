# From-scratch UI: text, layout, and interaction (viewer E2)

The viewer's control panel is a small **immediate-mode** UI built from scratch on the RHI (no Dear
ImGui; see [ADR-0015](../adr/0015-imgui-free-ui.md)). This note derives the three bits of "math": mapping
screen pixels to clip space, sampling a bitmap font from an atlas, and the layout + hit-testing that make
widgets place and react to themselves each frame. Code: `ui.hpp`, `ui_font.hpp`, `ui_render.hpp`,
`shaders/ui.{vert,frag}`.

## Screen pixels → clip space

UI geometry is authored in **screen pixels**, origin top-left, $y$ down — the natural coordinates for a
panel. Vulkan's normalized device coordinates (NDC) span $[-1,1]^2$ with **$y$ pointing down** too, so
the map is a straight affine per axis (no flip):

$$ x_\text{ndc} = \frac{2\,x_\text{px}}{W} - 1, \qquad y_\text{ndc} = \frac{2\,y_\text{px}}{H} - 1, $$

with $(W,H)$ the framebuffer size passed as a push constant. So pixel $(0,0)$ → $(-1,-1)$ (top-left) and
$(W,H)$ → $(1,1)$ (bottom-right). Depth is 0 and the depth test is off — the UI is an overlay drawn after
the 3-D scene (`load_op = Load`).

## Bitmap font atlas

The font is a built-in **5×7** glyph set authored as string-art (`ui_font.hpp`) and rasterised into one
**R-coverage** atlas: a grid of $16$ columns of $8{\times}8$ **cells** (the $5{\times}7$ glyph in the
top-left, the rest padding), covering printable ASCII from code point `kFirst = 32`. A character $p$ maps
to cell $p - 32$, at grid column $c = (p{-}32) \bmod 16$ and row $r = \lfloor (p{-}32)/16 \rfloor$. Its
texcoord rectangle (atlas size $A_w{\times}A_h$) is

$$ u_0 = \frac{c\cdot 8}{A_w},\; v_0 = \frac{r\cdot 8}{A_h}, \qquad
   u_1 = u_0 + \frac{5}{A_w},\; v_1 = v_0 + \frac{7}{A_h}, $$

so only the glyph's $5{\times}7$ region is sampled, not the cell padding. Each glyph is one quad; the pen
advances $(5+1)\,\text{scale}$ pixels per character (monospace, one-pixel gap). Lower-case code points
are folded onto their upper-case glyph at atlas-build time, so a label can be passed in any case.

**No blending, so alpha-test.** The RHI has no alpha blend yet, so the fragment shader computes coverage
— $1$ for a solid widget quad (flagged by $u<0$), or the atlas red channel for a glyph — and `discard`s
where coverage $< 0.5$. The panel is therefore opaque, the panel shows through the glyph gaps, and the
$0.5$ threshold on the linearly-filtered atlas gives a crisp edge at the glyph boundary. An SDF atlas +
blending (post-M5) is the smooth-scaling upgrade.

## Immediate-mode layout

There is no retained widget tree. `panel(x,y,w,h)` sets a **layout cursor** at the panel's inset
top-left; each widget draws at the cursor and advances it **down** by its row height
$\text{row} = 7\,s + \text{gap} + \text{pad}$ (a slider is taller — a label line plus a track band). So a
column of widgets simply falls out of the call order — the part checkboxes are a `for` loop. The whole
batch is appended to one vertex list in back-to-front order (panel background first, then widgets, then
text), which, with the depth test off, is correct painter's order without blending.

## Hit-testing and the hot/active idiom

Interaction is resolved from this frame's mouse state plus one bit of retained state. Define the **press
edge** (a click that began this frame) and **release edge**:

$$ \text{press} = \text{down} \wedge \neg\text{down}_\text{prev}, \qquad
   \text{release} = \neg\text{down} \wedge \text{down}_\text{prev}. $$

A widget is *hit* when the cursor lies in its rectangle. Then:

- **button** fires when `hit ∧ press`.
- **checkbox** flips its bound bool on `hit ∧ press`.
- **slider** uses the **active-item** rule: a press inside the track claims the slider
  (`active_ = id`); while it stays active *and the button is held* the value tracks the cursor,
  $\;v = \mathrm{lo} + \mathrm{clamp}\!\big(\tfrac{x_\text{mouse}-x}{w},0,1\big)(\mathrm{hi}-\mathrm{lo})$,
  even if the cursor leaves the track; the release edge clears `active_`. This single retained id is the
  whole of the UI's "memory" between frames.

The app suppresses camera orbit/pan/zoom while the cursor is over the panel **or** a widget is active
(`is_active()`), so dragging a slider never also spins the model.

## Verification

`tests/rhi/ui_offscreen_test` proves both halves — interaction GPU-free (button/checkbox/slider), and the
render off-screen (an opaque panel, bright text + knob pixels confirming the atlas path, a green checkbox
tick). End to end, the `--assembly` control panel toggles the tokamak's parts and scrubs the explode
slider, with the camera correctly deferring to the UI.
