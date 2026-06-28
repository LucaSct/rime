// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// A from-scratch **immediate-mode** UI for the viewer (E2). No retained widget tree and no
// third-party (no Dear ImGui): each frame the app calls begin(), then
// panel()/label()/button()/checkbox()/slider(), then end(); the widgets both *lay themselves out*
// (a simple vertical stack inside a panel) and *react* to the mouse this frame, appending coloured
// + textured quads to one vertex batch the UI renderer draws in a single pass over the 3-D scene.
// Interaction uses the classic hot/active-item idiom: a press inside a widget claims it
// (`active_`), so a slider keeps tracking the mouse while dragged even when the cursor leaves the
// track. Built only on rime::core math + the bitmap font (ui_font.hpp); the GPU side is
// ui_render.hpp. See docs/math/ui-text-layout.md.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ui_font.hpp"

namespace rime::viewer::ui {

// One UI vertex: screen-pixel position, atlas texcoord (u < 0 ⇒ a solid quad, no texture), RGBA
// colour.
struct UiVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct Color {
    float r, g, b, a = 1.0f;
};

// Immediate-mode context. Create one (it remembers the dragged widget between frames); call
// begin/end each frame around the widget calls.
class Ui {
public:
    // Start a frame: the framebuffer size and the current mouse state. Clears last frame's
    // geometry.
    void begin(float screen_w, float screen_h, float mouse_x, float mouse_y, bool mouse_down) {
        screen_w_ = screen_w;
        screen_h_ = screen_h;
        mouse_x_ = mouse_x;
        mouse_y_ = mouse_y;
        press_ = mouse_down && !prev_down_; // rising edge: a click began this frame
        release_ = !mouse_down && prev_down_;
        down_ = mouse_down;
        verts_.clear();
        next_id_ = 0;
        if (release_)
            active_ = 0; // letting go releases any dragged widget
    }

    void end() { prev_down_ = down_; }

    [[nodiscard]] const std::vector<UiVertex>& vertices() const { return verts_; }

    [[nodiscard]] float screen_w() const { return screen_w_; }

    [[nodiscard]] float screen_h() const { return screen_h_; }

    // True while a widget is being dragged (e.g. a slider) — the app suppresses camera input then.
    [[nodiscard]] bool is_active() const { return active_ != 0; }

    // Helpers for building custom widgets out of quad()/text() (e.g. the provenance list rows): is the
    // cursor over this rect, and was it clicked (pressed) inside it this frame?
    [[nodiscard]] bool hovered_in(float x, float y, float w, float h) const { return hit(x, y, w, h); }
    [[nodiscard]] bool clicked_in(float x, float y, float w, float h) const {
        return press_ && hit(x, y, w, h);
    }

    // --- layout: open a panel; subsequent widgets stack down its inside ---
    void panel(float x, float y, float w, float h, Color bg = {0.10f, 0.11f, 0.14f, 1.0f}) {
        quad(x, y, w, h, bg);
        panel_x_ = x;
        panel_w_ = w;
        cursor_x_ = x + kPad;
        cursor_y_ = y + kPad;
    }

    void label(const std::string& s, Color col = {0.85f, 0.87f, 0.92f, 1.0f}) {
        text(cursor_x_, cursor_y_, s, kTextScale, col);
        cursor_y_ += kRow;
    }

    // A button: returns true on the frame it is clicked (pressed inside). Highlights under the
    // cursor.
    [[nodiscard]] bool button(const std::string& s) {
        const float w = panel_w_ - 2.0f * kPad, h = kRow - kGap;
        const float x = cursor_x_, y = cursor_y_;
        const bool over = hit(x, y, w, h);
        const bool clicked = over && press_;
        quad(
            x, y, w, h, over ? Color{0.30f, 0.40f, 0.55f, 1.0f} : Color{0.20f, 0.23f, 0.30f, 1.0f});
        text(x + kPad,
             y + (h - kGlyphH * kTextScale) * 0.5f,
             s,
             kTextScale,
             {0.92f, 0.94f, 0.98f, 1.0f});
        cursor_y_ += kRow;
        return clicked;
    }

    // A checkbox bound to `value`: a box (filled when on) + label. Returns true the frame it
    // toggles.
    bool checkbox(const std::string& s, bool& value) {
        const float h = kRow - kGap;
        const float x = cursor_x_, y = cursor_y_;
        const float box = kGlyphH * kTextScale;
        const bool over = hit(x, y, panel_w_ - 2.0f * kPad, h);
        const bool toggled = over && press_;
        if (toggled)
            value = !value;
        quad(x, y, box, box, {0.45f, 0.48f, 0.55f, 1.0f});                             // box border
        quad(x + 1.0f, y + 1.0f, box - 2.0f, box - 2.0f, {0.12f, 0.13f, 0.16f, 1.0f}); // inset
        if (value)
            quad(x + 2.0f, y + 2.0f, box - 4.0f, box - 4.0f, {0.45f, 0.78f, 0.50f, 1.0f}); // tick
        text(x + box + kPad,
             y,
             s,
             kTextScale,
             over ? Color{1.0f, 1.0f, 1.0f, 1.0f} : Color{0.85f, 0.87f, 0.92f, 1.0f});
        cursor_y_ += kRow;
        return toggled;
    }

    // A horizontal slider bound to `value` in [lo, hi]: a label (with the current value) on top, a
    // track below it that you drag to set the value. Returns true on the frames it changes.
    bool slider(const std::string& s, float& value, float lo, float hi) {
        const std::uint32_t id = ++next_id_;
        const float x = cursor_x_, y = cursor_y_, w = panel_w_ - 2.0f * kPad;
        const float label_h = kGlyphH * kTextScale;
        const float track_y = y + label_h + 4.0f;
        const float row_h = label_h + 4.0f + 8.0f; // label + gap + track band
        if (hit(x, y, w, row_h) && press_)
            active_ = id;

        bool changed = false;
        if (active_ == id && down_ && hi > lo) {
            float t = (mouse_x_ - x) / w;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float nv = lo + t * (hi - lo);
            if (nv != value) {
                value = nv;
                changed = true;
            }
        }
        const float frac = (hi > lo) ? (value - lo) / (hi - lo) : 0.0f;
        const std::string val = std::to_string(static_cast<int>(value + 0.5f));
        text(x, y, s, kTextScale, {0.85f, 0.87f, 0.92f, 1.0f}); // label, left
        text(x + w - text_width(val, kTextScale),
             y,
             val,
             kTextScale,
             {0.70f, 0.80f, 0.95f, 1.0f});                             // value, right
        quad(x, track_y, w, 4.0f, {0.22f, 0.24f, 0.30f, 1.0f});        // track
        quad(x, track_y, w * frac, 4.0f, {0.35f, 0.55f, 0.80f, 1.0f}); // fill
        quad(x + w * frac - 3.0f, track_y - 5.0f, 6.0f, 14.0f, {0.85f, 0.88f, 0.94f, 1.0f}); // knob
        cursor_y_ += row_h + kGap;
        return changed;
    }

    [[nodiscard]] float cursor_y() const { return cursor_y_; }

    // --- primitives (also usable directly, e.g. the legend tick labels) ---

    // A solid-colour rectangle (u = −1 tells the shader "no texture").
    void quad(float x, float y, float w, float h, Color c) {
        push(x, y, -1.0f, -1.0f, c);
        push(x + w, y, -1.0f, -1.0f, c);
        push(x + w, y + h, -1.0f, -1.0f, c);
        push(x, y, -1.0f, -1.0f, c);
        push(x + w, y + h, -1.0f, -1.0f, c);
        push(x, y + h, -1.0f, -1.0f, c);
    }

    // A straight line of pixel `thickness` between two screen points, drawn as a thin quad along the
    // segment's perpendicular. The gas-path chart's axes and curves use it (the UI is otherwise all
    // axis-aligned quads); u = −1 marks it untextured for the shader, like quad().
    void line(float x0, float y0, float x1, float y1, float thickness, Color c) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0e-6f)
            return;
        const float nx = -dy / len * thickness * 0.5f; // half-width along the perpendicular
        const float ny = dx / len * thickness * 0.5f;
        push(x0 + nx, y0 + ny, -1.0f, -1.0f, c);
        push(x1 + nx, y1 + ny, -1.0f, -1.0f, c);
        push(x1 - nx, y1 - ny, -1.0f, -1.0f, c);
        push(x0 + nx, y0 + ny, -1.0f, -1.0f, c);
        push(x1 - nx, y1 - ny, -1.0f, -1.0f, c);
        push(x0 - nx, y0 - ny, -1.0f, -1.0f, c);
    }

    // Draw a string as one textured quad per glyph, sampled from the font atlas. Monospace;
    // lower-case renders as upper-case (the atlas folds it). Returns the pen x after the string.
    float text(float x, float y, const std::string& s, float scale, Color c) {
        const float gw = kGlyphW * scale, gh = kGlyphH * scale;
        const float adv = (kGlyphW + 1) * scale;
        float pen = x;
        for (char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            if (u >= kFirst && u <= kLast && ch != ' ') {
                const std::uint32_t cell = u - kFirst;
                const std::uint32_t col = cell % kCols, row = cell / kCols;
                const float u0 = static_cast<float>(col * kCell) / kAtlasW;
                const float v0 = static_cast<float>(row * kCell) / kAtlasH;
                const float u1 = u0 + static_cast<float>(kGlyphW) / kAtlasW;
                const float v1 = v0 + static_cast<float>(kGlyphH) / kAtlasH;
                push(pen, y, u0, v0, c);
                push(pen + gw, y, u1, v0, c);
                push(pen + gw, y + gh, u1, v1, c);
                push(pen, y, u0, v0, c);
                push(pen + gw, y + gh, u1, v1, c);
                push(pen, y + gh, u0, v1, c);
            }
            pen += adv;
        }
        return pen;
    }

    [[nodiscard]] static float text_width(const std::string& s, float scale) {
        return static_cast<float>(s.size()) * (kGlyphW + 1) * scale;
    }

private:
    void push(float x, float y, float u, float v, Color c) {
        verts_.push_back({x, y, u, v, c.r, c.g, c.b, c.a});
    }

    [[nodiscard]] bool hit(float x, float y, float w, float h) const {
        return mouse_x_ >= x && mouse_x_ <= x + w && mouse_y_ >= y && mouse_y_ <= y + h;
    }

    static constexpr float kTextScale = 2.0f; // a 5×7 glyph drawn at 10×14 px
    static constexpr float kPad = 8.0f;       // panel inner padding
    static constexpr float kGap = 6.0f;       // gap between a widget and the next row
    static constexpr float kRow = kGlyphH * kTextScale + kGap + 8.0f; // one widget row height

    std::vector<UiVertex> verts_;
    float screen_w_ = 1.0f, screen_h_ = 1.0f;
    float mouse_x_ = 0.0f, mouse_y_ = 0.0f;
    bool down_ = false, prev_down_ = false, press_ = false, release_ = false;
    std::uint32_t active_ = 0, next_id_ = 0;
    float panel_x_ = 0.0f, panel_w_ = 0.0f, cursor_x_ = 0.0f, cursor_y_ = 0.0f;
};

} // namespace rime::viewer::ui
