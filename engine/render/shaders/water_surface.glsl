// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#ifndef RIME_WATER_SURFACE_GLSL
#define RIME_WATER_SURFACE_GLSL

struct PhaseOptics {
    vec3 normal;
    vec3 f0;
    vec3 transmission;
    float roughness;
    // Inputs to the screen-space refraction path (WATER_PASS only): the absorption coefficient
    // per metre, the in-scattered body colour, the authored water column that exists even when
    // the floor mesh is flush with the sheet, and how milky the medium is (ice inclusions).
    vec3 sigma;
    vec3 body;
    vec3 milk;
    float virtual_depth;
    float cloudy;
};


// Water slope field: a sum of travelling sine waves, one per octave of wavelength.
//
// A heightfield h(p,t) = sum a_i sin(k_i.p - w_i t) has slope grad h = sum a_i k_i cos(...) dir_i,
// so we sum SLOPES directly and never need the height. Each term carries a target slope (the
// steepness a_i k_i, dimensionless), which is what the eye reads as glitter: rms ~0.09 is a
// breezy pond. Frequencies follow the capillary-gravity dispersion w = sqrt(g k (1 + (k/370)^2)),
// so long waves crawl and short ripples dart, which is what makes the motion look like water
// rather than a texture scrolling. Directions fan +-40 degrees around the wind, and phases are
// irrational offsets so no two terms line up into a visible lattice.
//
// Anti-aliasing: a wave shorter than ~4 pixels of footprint only produces shimmering noise, so each
// term fades out as wavelength/footprint falls. The residual roughness is supplied by the
// specular filter, which is the same energy the faded waves would have carried.
vec2 water_slope(vec2 p, float t, float footprint) {
    const float kWavelength[8] = float[](4.1, 2.3, 1.27, .73, .41, .235, .135, .078);
    const float kSlope[8] = float[](.020, .030, .042, .052, .056, .050, .040, .030);
    const float kAngle[8] = float[](.00, .55, -.62, .31, -.28, .70, -.74, .12);
    vec2 s = vec2(0.0);
    for (int i = 0; i < 8; ++i) {
        float k = 6.2831853 / kWavelength[i];
        float w = sqrt(9.81 * k * (1.0 + (k / 370.0) * (k / 370.0)));
        float ang = .3398 + kAngle[i]; // wind direction (0.94, 0.34) plus the fan offset
        vec2 d = vec2(cos(ang), sin(ang));
        float resolved = smoothstep(2.0, 8.0, kWavelength[i] / footprint);
        s += d * (kSlope[i] * resolved * cos(k * dot(d, p) - w * t + float(i) * 1.7));
        // A crossing train at a different angle and speed breaks the straight crests into chop.
        vec2 e = vec2(cos(ang + 1.9), sin(ang + 1.9));
        s += e * (kSlope[i] * .55 * resolved *
                  cos(k * 1.13 * dot(e, p) - w * 0.83 * t + float(i) * 2.9 + 1.0));
    }
    return s;
}

// ── ICE FRACTURES ───────────────────────────────────────────────────────────────────────────
//
// A frozen pond cracks along stress lines that are long, thin, sparse and irregular, at several
// scales at once, and some of them run DEEP inside the slab. Three ingredients here:
//
// * Fractures are the EDGES of a Voronoi diagram (distance to the bisector of the two nearest
//   seeds, (d2 - d1)), but only edges a per-pair hash selects survive -- sparse and branching,
//   instead of the uniform tile grid a full diagram gives. Three scales, thinner and rarer as
//   they get finer, and the plane is domain-warped by low-frequency noise so no edge is straight.
// * Depth by parallax. A crack at depth d below the surface appears shifted along the view ray by
//   d/cos(theta). Evaluating the SAME field at a view-shifted position gives deep layers that
//   slide against the surface as the camera moves, the cheap cue the eye reads as thickness.
// * Air bubbles freeze into long white streaks aligned with the growth direction: stripes in a
//   rotated frame, gated per stripe and broken into segments by value noise.
//
// Edge widths are clamped to the pixel footprint (and faded by how far below it they fall) so thin
// lines shimmer rather than alias.
float ihash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec2 ihash2(vec2 p) {
    return vec2(ihash(p), ihash(p + 19.19));
}

float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(ihash(i), ihash(i + vec2(1, 0)), f.x),
               mix(ihash(i + vec2(0, 1)), ihash(i + vec2(1, 1)), f.x), f.y);
}

float crack_layer(vec2 p, float scale, float gate, float width, float footprint, float seed) {
    vec2 q = p * scale + seed;
    q += (vec2(vnoise(q * .55 + 3.1), vnoise(q * .55 + 9.7)) - .5) * .9; // irregular, not straight
    vec2 ip = floor(q), fp = fract(q);
    float d1 = 8.0, d2 = 8.0;
    vec2 id1 = vec2(0.0), id2 = vec2(0.0);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(i, j);
            vec2 r = g + ihash2(ip + g) - fp;
            float d = dot(r, r);
            if (d < d1) { d2 = d1; id2 = id1; d1 = d; id1 = ip + g; }
            else if (d < d2) { d2 = d; id2 = ip + g; }
        }
    float edge_m = (sqrt(d2) - sqrt(d1)) * .5 / scale; // metres to the nearest edge, roughly
    float keep = step(ihash(id1 + id2 + seed), gate);
    float w = max(width, footprint * .9);
    float strength = clamp(width / max(footprint, 1e-5), .25, 1.0);
    return keep * strength * (1.0 - smoothstep(w * .35, w, edge_m));
}

// x = fracture brightness (surface + two deep layers), y = bubble streaks, z = surface scratches.
vec3 ice_fractures(vec2 p, vec3 view, float footprint) {
    vec2 shift = -view.xz / max(view.y, .15);
    float top = max(crack_layer(p, .9, .50, .0030, footprint, 1.0),
                    max(crack_layer(p, 2.6, .38, .0016, footprint, 7.0),
                        crack_layer(p, 7.0, .24, .0009, footprint, 13.0) * .8));
    float deep1 = max(crack_layer(p + shift * .04, 1.2, .45, .0045, footprint, 21.0),
                      crack_layer(p + shift * .04, 3.4, .30, .0030, footprint, 27.0) * .8);
    float deep2 = crack_layer(p + shift * .11, .7, .40, .0070, footprint, 33.0);
    float crack = clamp(max(top, max(deep1 * .55, deep2 * .32)), 0.0, 1.0);

    vec2 q = vec2(dot(p, vec2(.9, .43)), dot(p, vec2(-.43, .9)));
    float row = floor(q.y * 14.0);
    float h = ihash(vec2(row, 3.1));
    float along = vnoise(vec2(q.x * (2.0 + h * 3.0) + h * 50.0, row));
    float across = 1.0 - smoothstep(.10, .34, abs(fract(q.y * 14.0) - .5));
    float streaks = step(.86, h) * smoothstep(.62, .85, along) * across * .55;

    vec2 sc = vec2(dot(p, vec2(.8, .6)) * 38.0, dot(p, vec2(-.6, .8)) * 1.3);
    float scratch = smoothstep(.72, .90, vnoise(sc)) * smoothstep(.5, .8, vnoise(p * 2.3 + 5.0));
    return vec3(crack, streaks, scratch);
}

PhaseOptics phase_optics(vec3 geometric, vec3 detail_normal, vec3 tangent, vec3 position,
                        vec2 uv, vec3 view, vec3 tint, vec3 frost_map,
                        float authored_roughness, vec4 phase, vec3 presentation) {
    PhaseOptics o;
    float wet = phase.y > 2.5 ? 0.0 : phase.z;
    float frost = phase.y > 2.5 ? 1.0 : 0.0;
    vec3 N = normalize(geometric);
    vec3 T = normalize(tangent - N * dot(N, tangent));
    vec3 B = normalize(cross(N, T));
    // Living surface: a spectrum of travelling slope waves (see water_slope above). Static frost
    // slopes still fade with the exact liquid fraction, avoiding the old 1/8 atlas jumps. The
    // clock is committed simulation time, so paused/refused ticks remain still.
    vec2 p = position.xz;
    float footprint = max(length(fwidth(position)), 1e-4);
    vec2 slope = water_slope(p, presentation.x, footprint);
    vec3 mapped = detail_normal / max(dot(detail_normal, N), .25) - N;
    o.normal = normalize(N + mapped * (frost > .5 ? .18 : mix(.025, .06, wet)) +
                         wet * vec3(-slope.x, 0.0, -slope.y));
    // Grazing guard. At a low camera the steep side of a wave tilts the mirror direction BELOW the
    // surface, where the reflection resolves to the dark "ground" half of the sky and the crest
    // reads as a black hole. Real waves self-shadow there; we instead pull the normal toward the
    // flat one until the reflected ray clears the horizon of the sheet.
    for (int guard = 0; guard < 2; ++guard) {
        float up = dot(reflect(-view, o.normal), N);
        o.normal = normalize(mix(o.normal, N, clamp((.06 - up) * 5.0, 0.0, 1.0)));
    }
    // Ice, from the inside out. All of it is procedural in WORLD metres (not uv), so cracks keep
    // their size on any sheet and never repeat as a visible tile. See ice_fractures above.
    vec3 ice = ice_fractures(p, view, footprint);
    float crack = ice.x, streaks = ice.y, scratch = ice.z;
    float frost_patch = smoothstep(.62, .85, vnoise(p * .9 + 11.0) * .6 + vnoise(p * 2.9 - 4.0) * .4) * .5;
    float cloudy = clamp(.08 + crack * .85 + streaks * .55 + frost_patch * .3 + frost, 0.0, 1.0) *
                   (1.0 - wet);
    // Polished ice is nearly a mirror (sharp sky reflection); scratches and frost patches scatter.
    float ice_roughness = clamp(.045 + scratch * .22 + frost_patch * .28 + crack * .12, .04, .5);
    // Keep the authored map responsive (including the environment smoke's roughness control),
    // while ordinary liquid texels land in the requested 0.02–0.06 glint range.
    float water_roughness = clamp(authored_roughness * .45, .025, .45);
    o.roughness = mix(mix(ice_roughness, .68, frost), water_roughness, wet);
    o.f0 = vec3(mix(.018, .020, wet));
    // Beer–Lambert absorption: optical depth is metres / refracted-ray cosine (Snell).
    // Red is absorbed fastest, leaving a cool blue-green column; a tapered bank makes
    // thin edges approach the authored bottom colour. This is a bottom-colour proxy,
    // not a scene-colour refraction pass: submerged geometry is not sampled here.
    vec2 edge_uv = uv * 2.0 - 1.0;
    float edge = presentation.y > .5 ? max(0.0, 1.0 - dot(edge_uv, edge_uv))
                                     : max(0.0, (1.0-edge_uv.x*edge_uv.x) *
                                                 (1.0-edge_uv.y*edge_uv.y));
    float thickness = phase.w * smoothstep(0.0, .55, edge);
    float nv = clamp(dot(o.normal, view), 0.0, 1.0);
    float cos_refracted = sqrt(1.0 - (1.0 - nv * nv) / (1.333 * 1.333));
    vec3 sigma = mix(vec3(6.5, 2.4, 1.2), vec3(3.2, .85, .45), wet);
    vec3 attenuation = exp(-sigma * thickness / cos_refracted);
    vec3 body = mix(vec3(.12, .30, .38), vec3(.012, .085, .12), wet);
    vec3 transmitted = tint * attenuation + body * (1.0 - attenuation);
    transmitted = mix(transmitted, vec3(.72, .84, .87), cloudy * .65);
    // Schlick energy split: the transmitted/scattered share is exactly the complement
    // of the reflected share. The specular lobe uses F0 through the existing sky/SSR path.
    o.transmission = transmitted * (vec3(1.0) - f_schlick(nv, o.f0));
    o.sigma = sigma;
    o.body = body;
    o.milk = vec3(.72, .84, .87);
    o.virtual_depth = thickness;
    o.cloudy = cloudy;
    return o;
}

#ifdef WATER_PASS
// ── SCREEN-SPACE REFRACTION ─────────────────────────────────────────────────────────────────
//
// Water is mostly TRANSPARENT: what you see is the floor, bent by refraction, dimmed and tinted by
// the water column, with the sky mirrored on top in proportion to Fresnel. A bottom-colour proxy
// cannot show that, so this pass draws water AFTER the opaque scene and reads a copy of the
// finished scene colour and depth (scene_copy.frag; a pass cannot sample what it renders into).
//
// 1. Thickness. The depth copy holds the opaque floor, because water is excluded from the
//    pre-pass. Floor point B and surface point P give the vertical water column h = (P-B).N. A
//    sheet laid flush on its floor measures ~0, so the authored depth is a lower bound: the shader
//    uses max(measured, authored). At the bank the authored depth tapers to zero, so edges are
//    clear and depth darkens naturally.
// 2. Refraction. Snell's law bends the view ray into the water (eta = 1/1.333). The refracted ray
//    reaches the floor after h / cos(theta_r) along its length; that point Q projects to a screen
//    position, and the opaque colour THERE is what the camera sees through the surface. Rippled
//    normals bend the ray differently per pixel, which is the wobble.
// 3. Rejection. If the colour at Q's screen position belongs to something in FRONT of the water
//    (a post, the bank wall) the lookup would smear foreground onto the water, so the unbent pixel
//    is used instead.
// 4. Beer-Lambert. Light dims exponentially with path length, per channel: T = exp(-sigma * path).
//    Red dies first, leaving the blue-green of deep water; in-scattered light fills in the part
//    the column absorbed, so a deep pond tends to its body colour rather than to black.
// 5. Caustics. The wavy surface focuses sunlight on the floor into bright moving filaments, faked
//    here with two crossing ridge patterns that drift with the waves, fading with depth.
vec3 water_unproject(mat4 inv_vp, vec2 uv, float depth) {
    vec4 c = inv_vp * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return c.xyz / c.w;
}

float water_caustic(vec2 p, float t) {
    vec2 a = p * 5.3 + vec2(sin(p.y * 3.1 + t * .9), cos(p.x * 2.7 - t * .7));
    vec2 b = p * 8.1 + vec2(cos(p.y * 4.3 - t * 1.3), sin(p.x * 3.9 + t * 1.1));
    float c1 = 1.0 - abs(sin(a.x + sin(a.y)));
    float c2 = 1.0 - abs(sin(b.x + sin(b.y)));
    return pow(c1 * c2, 3.0);
}

vec3 water_refracted_radiance(PhaseOptics o, vec3 P, vec3 Ngeo, vec3 v, vec3 incident,
                              float sun_visibility, float time) {
    vec2 size = vec2(textureSize(water_scene_color, 0));
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / size;
    mat4 inv_vp = inverse(frame.view_proj);
    float d0 = uintBitsToFloat(texelFetch(water_scene_depth, px, 0).r);
    bool has_floor = d0 < .99999;
    vec3 B0 = water_unproject(inv_vp, uv, d0);
    float h_scene = has_floor ? max(dot(P - B0, Ngeo), 0.0) : 8.0;
    float h = max(h_scene, o.virtual_depth);
    vec3 r = refract(-v, o.normal, 1.0 / 1.333);
    float cos_r = max(dot(-r, Ngeo), .05);
    float path = h / cos_r;
    vec3 Q = P + r * path;
    vec4 cq = frame.view_proj * vec4(Q, 1.0);
    vec2 uvq = clamp(cq.xy / cq.w * .5 + .5, vec2(.002), vec2(.998));
    float dq = uintBitsToFloat(texelFetch(water_scene_depth, ivec2(uvq * size), 0).r);
    vec3 Bq = water_unproject(inv_vp, uvq, dq);
    float eye_to_surface = distance(frame.camera_pos.xyz, P);
    if (has_floor && distance(frame.camera_pos.xyz, Bq) < eye_to_surface - .03)
        uvq = uv; // foreground in the way: fall back to the unbent lookup
    vec3 bottom = texture(water_scene_color, uvq).rgb;
    float shallow = 1.0 - smoothstep(.0, .6, h);
    // Light reaching the floor is also attenuated on its way down; caustics live in that light.
    float caustic = water_caustic(Q.xz, time) * sun_visibility * shallow;
    bottom *= 1.0 + caustic * 1.6;
    vec3 T = exp(-o.sigma * path);
    vec3 col = bottom * T + o.body * incident * (1.0 - T);
    col = mix(col, o.milk * incident * .6 + bottom * T * .25, o.cloudy * .65);
    return col;
}
#endif
#endif
