// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// codec_bench — the measurement behind ADR-0017 (Track S / S0.3).
//
// The streaming plan says: choose the codec *by measurement*, and record the numbers. This is that
// measurement. It is GPU-free — it synthesizes representative frames on the CPU and runs them
// through rime::stream's FrameEncoder/FrameDecoder — so it runs anywhere, including the headless
// Linux server that is the S0 streaming test bed.
//
// It reports, per (content, codec): compression ratio, encode/decode throughput, the resulting
// on-the-wire bandwidth at 30 fps (the number the network budget cares about), and — for the lossy
// JPEG path — the PSNR so the quality cost is quantified, not hand-waved. Content matters
// enormously for a lossless codec, so we test four frames spanning the spectrum a real viewport
// produces: a smooth gradient, a shaded "3-D scene", a flat-with-sharp-edges "UI", and
// incompressible noise.
//
// Usage: codec_bench [width height] [frames]   (defaults: 1280x720, 60 frames)

#include <fmt/core.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rime/stream/frame_codec.hpp"
#include "rime/stream/video_codec.hpp"

using namespace rime;
using Bytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;

namespace {

std::byte b(int v) {
    return static_cast<std::byte>(static_cast<std::uint8_t>(v & 0xFF));
}

int u8(std::byte x) {
    return static_cast<int>(std::to_integer<std::uint8_t>(x));
}

void put(Bytes& px, std::uint32_t w, std::uint32_t x, std::uint32_t y, int r, int g, int bl) {
    std::byte* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
    p[0] = b(r);
    p[1] = b(g);
    p[2] = b(bl);
    p[3] = b(255);
}

// A smooth gradient — low spatial frequency, the best case for both codecs.
Bytes make_gradient(std::uint32_t w, std::uint32_t h) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            put(px,
                w,
                x,
                y,
                static_cast<int>(x * 255u / w),
                static_cast<int>(y * 255u / h),
                static_cast<int>((x + y) * 128u / w));
    return px;
}

// A shaded "3-D scene": a sky gradient, a ground band, and a few radially-shaded spheres. Mid-
// frequency, the kind of content a game/editor viewport actually streams.
Bytes make_scene(std::uint32_t w, std::uint32_t h) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    const std::uint32_t horizon = h * 2u / 3u;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (y < horizon) {
                const int t = static_cast<int>(y * 255u / horizon);
                put(px, w, x, y, 90 + t / 3, 130 + t / 3, 210 - t / 4); // sky
            } else {
                put(px, w, x, y, 60, 90 + static_cast<int>((x * 20u) / w), 50); // ground
            }
        }
    }

    // A handful of shaded spheres.
    const struct {
        std::uint32_t cx, cy, r;
        int cr, cg, cb;
    } spheres[] = {
        {w / 4u, h / 2u, h / 6u, 200, 60, 60},
        {w / 2u, h / 2u, h / 5u, 60, 200, 120},
        {3u * w / 4u, h / 2u, h / 7u, 120, 120, 220},
    };

    for (const auto& s : spheres) {
        for (std::uint32_t y = (s.cy > s.r ? s.cy - s.r : 0); y < s.cy + s.r && y < h; ++y) {
            for (std::uint32_t x = (s.cx > s.r ? s.cx - s.r : 0); x < s.cx + s.r && x < w; ++x) {
                const double dx = double(x) - s.cx, dy = double(y) - s.cy;
                const double d2 = dx * dx + dy * dy;
                if (d2 <= double(s.r) * s.r) {
                    const double shade = 1.0 - std::sqrt(d2) / s.r * 0.7; // radial falloff
                    put(px, w, x, y, int(s.cr * shade), int(s.cg * shade), int(s.cb * shade));
                }
            }
        }
    }
    return px;
}

// A "UI" frame: a flat dark background, lighter panels, and sharp high-contrast bars (a stand-in
// for text). Big flat regions LZ4 eats; the sharp edges are where JPEG rings.
Bytes make_ui(std::uint32_t w, std::uint32_t h) {
    Bytes px(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = b(24);
        px[i + 1] = b(26);
        px[i + 2] = b(30);
        px[i + 3] = b(255);
    }
    for (std::uint32_t y = 20; y < h - 20 && y < h; ++y) {
        for (std::uint32_t x = 20; x < w / 3u && x < w; ++x) {
            put(px, w, x, y, 48, 52, 60); // a panel
            if ((y % 16u) < 7u && x > 28 && x < w / 3u - 8) {
                put(px, w, x, y, 230, 232, 236); // bright "text" rows
            }
        }
    }
    return px;
}

// Peak signal-to-noise ratio (dB) between two equal-size byte images. Higher is better; identical
// images are "inf". Standard lossy-image quality metric.
double psnr(const Bytes& a, const Bytes& c) {
    double mse = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double e = double(u8(a[i]) - u8(c[i]));
        mse += e * e;
    }
    mse /= double(a.size());
    if (mse <= 0.0)
        return 1e9; // lossless
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

const char* codec_name(stream::Codec c) {
    switch (c) {
        case stream::Codec::Raw:
            return "raw";
        case stream::Codec::LZ4:
            return "lz4";
        case stream::Codec::Jpeg:
            return "jpeg";
        case stream::Codec::Av1:
            return "av1";
    }
    return "?";
}

struct Row {
    const char* content;
    stream::Codec codec;
    double ratio;       // raw / encoded
    double enc_mbps;    // raw MB/s pushed through the encoder
    double dec_mbps;    // raw MB/s reconstructed
    double wire_mbps30; // encoded bandwidth at 30 fps
    double psnr_db;     // JPEG quality (inf for lossless)
};

// Time `iters` encodes then `iters` decodes of one frame, reusing the encoder/decoder (as a real
// stream would). Returns a filled Row.
Row bench(const char* content,
          stream::Codec codec,
          const stream::ImageDesc& desc,
          const Bytes& src,
          int iters,
          int jpeg_quality) {
    stream::FrameEncoder enc;
    enc.set_options({jpeg_quality});
    stream::FrameDecoder dec;
    Bytes wire;
    Bytes out(desc.byte_size());

    bool ok =
        enc.encode(codec, desc, src, wire); // warm up (first JPEG frame lazily inits the handle)

    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        ok = enc.encode(codec, desc, src, wire) && ok;
    const auto t1 = Clock::now();
    for (int i = 0; i < iters; ++i)
        ok = dec.decode(codec, desc, wire, out) && ok;
    const auto t2 = Clock::now();
    if (!ok)
        fmt::print("  (!) {} {} encode/decode reported an error\n", content, codec_name(codec));

    const double raw_mb = double(desc.byte_size()) / 1e6;
    const double enc_s = std::chrono::duration<double>(t1 - t0).count() / iters;
    const double dec_s = std::chrono::duration<double>(t2 - t1).count() / iters;

    Row r{};
    r.content = content;
    r.codec = codec;
    r.ratio = double(desc.byte_size()) / double(wire.size());
    r.enc_mbps = raw_mb / enc_s;
    r.dec_mbps = raw_mb / dec_s;
    r.wire_mbps30 = double(wire.size()) * 30.0 / 1e6;
    r.psnr_db = psnr(src, out);
    return r;
}

// ── AV1 (s1.2) ──────────────────────────────────────────────────────────────────────────────────
//
// AV1 is *stateful inter-frame* (VideoEncoder/VideoDecoder), so it does not fit the per-call
// bench() above and gets its own path. Two things matter for an honest measurement:
//
//   1. **The content must move.** A static frame repeated is AV1's trivial best case — deltas
//      near zero — and would flatter it meaninglessly. We scroll each base frame a few pixels per
//      frame, the temporal change a panning viewport actually produces, so inter-frame coding does
//      real work. (This is *harder* than a mostly-static editor viewport, so the bitrate here is a
//      pessimistic bound on the real win over per-frame JPEG.)
//   2. **Encode time includes RGBA->YUV conversion**, because it happens inside encode(). The plan
//      flags that conversion as "the classic hidden cost"; folding it into the timing is the whole
//      point of measuring rather than guessing.

// Scroll a frame horizontally by `dx` pixels (wrap-around) — a cheap way to synthesize temporal
// motion from one authored frame.
Bytes scroll(const Bytes& src, std::uint32_t w, std::uint32_t h, std::uint32_t dx) {
    Bytes out(src.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t sx = (x + dx) % w;
            std::memcpy(&out[(static_cast<std::size_t>(y) * w + x) * 4u],
                        &src[(static_cast<std::size_t>(y) * w + sx) * 4u],
                        4u);
        }
    }
    return out;
}

struct Av1Row {
    const char* content;
    int preset;
    bool multicore;
    double enc_ms;       // encode ms/frame (incl. RGBA->YUV conversion)
    double dec_ms;       // decode ms/frame (incl. YUV->RGBA)
    double wire_mbps30;  // encoded bandwidth at 30 fps
    double bitrate_kbps; // the same number the rate controller targets
    double psnr_db;      // reconstructed quality vs the (moving) source
};

// Encode `frames` scrolled variants of `base` through one VideoEncoder, decode them back, and
// summarize. Returns a filled Av1Row; on any codec failure the row is left zeroed (and a note is
// printed), so a partial environment does not abort the whole bench.
Av1Row bench_av1(const char* content,
                 const stream::ImageDesc& desc,
                 const Bytes& base,
                 int frames,
                 int preset,
                 int target_kbps,
                 bool multicore) {
    Av1Row r{};
    r.content = content;
    r.preset = preset;
    r.multicore = multicore;

    stream::VideoEncoder enc;
    stream::VideoEncoder::Config cfg;
    cfg.desc = desc;
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.target_bitrate_kbps = static_cast<std::uint32_t>(target_kbps);
    cfg.preset = preset;
    cfg.parallelism = multicore ? 0u : 1u; // 0 = use the machine; 1 = single-core baseline
    if (!enc.open(cfg)) {
        fmt::print("  (!) av1 {} preset {} failed to open encoder\n", content, preset);
        return r;
    }
    stream::VideoDecoder dec;
    if (!dec.open({desc}, enc.sequence_header())) {
        fmt::print("  (!) av1 {} preset {} failed to open decoder\n", content, preset);
        return r;
    }

    std::size_t total_wire = 0;
    double enc_s = 0.0;
    double dec_s = 0.0;
    double psnr_sum = 0.0;
    int measured = 0;
    std::vector<stream::VideoPacket> packets;
    Bytes decoded;
    for (int i = 0; i < frames; ++i) {
        const Bytes src = scroll(base,
                                 desc.extent.width,
                                 desc.extent.height,
                                 static_cast<std::uint32_t>(i * 2)); // 2 px/frame pan

        const auto t0 = Clock::now();
        const bool enc_ok = enc.encode(src, packets);
        const auto t1 = Clock::now();
        if (!enc_ok || packets.size() != 1) {
            fmt::print("  (!) av1 {} preset {} encode error at frame {}\n", content, preset, i);
            break;
        }
        bool ready = false;
        const auto t2 = Clock::now();
        const bool dec_ok = dec.decode(packets[0].data, decoded, ready);
        const auto t3 = Clock::now();
        if (!dec_ok || !ready) {
            fmt::print("  (!) av1 {} preset {} decode error at frame {}\n", content, preset, i);
            break;
        }
        // Skip the keyframe (frame 0) from the steady-state averages: it is a full intra, far
        // bigger and slower than the delta frames that dominate a real stream.
        if (i > 0) {
            total_wire += packets[0].data.size();
            enc_s += std::chrono::duration<double>(t1 - t0).count();
            dec_s += std::chrono::duration<double>(t3 - t2).count();
            psnr_sum += psnr(src, decoded);
            ++measured;
        }
    }
    if (measured == 0) {
        return r;
    }
    const double avg_wire = static_cast<double>(total_wire) / measured;
    r.enc_ms = enc_s / measured * 1e3;
    r.dec_ms = dec_s / measured * 1e3;
    r.wire_mbps30 = avg_wire * 30.0 / 1e6;
    r.bitrate_kbps = avg_wire * 8.0 * 30.0 / 1e3;
    r.psnr_db = psnr_sum / measured;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t w = 1280, h = 720;
    int frames = 60;
    const int jpeg_quality = 80;
    if (argc >= 3) {
        w = static_cast<std::uint32_t>(std::atoi(argv[1]));
        h = static_cast<std::uint32_t>(std::atoi(argv[2]));
    }
    if (argc >= 4)
        frames = std::atoi(argv[3]);
    if (w == 0 || h == 0 || frames <= 0) {
        fmt::print("usage: codec_bench [width height] [frames]\n");
        return 2;
    }

    const stream::ImageDesc desc{{w, h}, rhi::Format::RGBA8Unorm};
    const double raw_mbps30 = double(desc.byte_size()) * 30.0 / 1e6;

    fmt::print("\ncodec_bench — {}x{}, {} frames/measurement, JPEG quality {}\n",
               w,
               h,
               frames,
               jpeg_quality);
    fmt::print("raw baseline: {:.1f} MB/frame, {:.0f} MB/s at 30 fps\n\n",
               double(desc.byte_size()) / 1e6,
               raw_mbps30);
    fmt::print("{:<9} {:<5} {:>8} {:>11} {:>11} {:>13} {:>9}\n",
               "content",
               "codec",
               "ratio",
               "enc MB/s",
               "dec MB/s",
               "wire MB/s@30",
               "PSNR dB");
    fmt::print("{:-<69}\n", "");

    const struct {
        const char* name;
        Bytes (*gen)(std::uint32_t, std::uint32_t);
    } contents[] = {
        {"gradient", make_gradient}, {"scene", make_scene}, {"ui", make_ui},
        // noise is generated inline below (it needs a seed, not the w,h-only signature)
    };

    auto print_row = [&](const Row& r) {
        if (r.psnr_db >= 1e8)
            fmt::print("{:<9} {:<5} {:>7.1f}x {:>11.0f} {:>11.0f} {:>13.2f} {:>9}\n",
                       r.content,
                       codec_name(r.codec),
                       r.ratio,
                       r.enc_mbps,
                       r.dec_mbps,
                       r.wire_mbps30,
                       "lossless");
        else
            fmt::print("{:<9} {:<5} {:>7.1f}x {:>11.0f} {:>11.0f} {:>13.2f} {:>9.1f}\n",
                       r.content,
                       codec_name(r.codec),
                       r.ratio,
                       r.enc_mbps,
                       r.dec_mbps,
                       r.wire_mbps30,
                       r.psnr_db);
    };

    const stream::Codec codecs[] = {stream::Codec::Raw, stream::Codec::LZ4, stream::Codec::Jpeg};
    for (const auto& c : contents) {
        const Bytes src = c.gen(w, h);
        for (stream::Codec codec : codecs)
            print_row(bench(c.name, codec, desc, src, frames, jpeg_quality));
        fmt::print("{:-<69}\n", "");
    }
    // Noise: the incompressible worst case for the lossless codecs.
    {
        Bytes src(desc.byte_size());
        std::uint32_t s = 0x1234567u;
        for (auto& byte : src) {
            s = s * 1664525u + 1013904223u;
            byte = b(static_cast<int>((s >> 24) & 0xFFu));
        }
        for (stream::Codec codec : codecs)
            print_row(bench("noise", codec, desc, src, frames, jpeg_quality));
        fmt::print("{:-<69}\n", "");
    }

    // ── AV1 (s1.2, ADR-0030) — stateful inter-frame video ──────────────────────────────────────
    // Encode a *moving* (scrolled) sequence so deltas do real work, then report the steady-state
    // (post-keyframe) cost. This is the confirming bench ADR-0030 §1 deferred to s1.2. Two presets
    // bracket the low-latency range; a single-core row shows how much SVT-AV1's multi-core encode
    // is buying on this box.
    const int av1_target_kbps = 4000;
    fmt::print("\nAV1 (SVT-AV1 enc / dav1d dec) — moving sequence, CBR target {} kbps, {} frames\n",
               av1_target_kbps,
               frames);
    fmt::print("{:<9} {:>6} {:>7} {:>9} {:>9} {:>13} {:>11} {:>9}\n",
               "content",
               "preset",
               "cores",
               "enc ms",
               "dec ms",
               "wire MB/s@30",
               "kbit/s",
               "PSNR dB");
    fmt::print("{:-<80}\n", "");

    auto print_av1 = [&](const Av1Row& r) {
        fmt::print("{:<9} {:>6} {:>7} {:>9.2f} {:>9.2f} {:>13.3f} {:>11.0f} {:>9.1f}\n",
                   r.content,
                   r.preset,
                   r.multicore ? "multi" : "1",
                   r.enc_ms,
                   r.dec_ms,
                   r.wire_mbps30,
                   r.bitrate_kbps,
                   r.psnr_db);
    };

    // scene + ui are the representative viewport content (a shaded 3-D scene, a flat UI). gradient
    // is the easy case. Preset 10 (higher quality) and 12 (faster) bracket the trade.
    const Bytes scene = make_scene(w, h);
    const Bytes ui = make_ui(w, h);
    for (int preset : {10, 12}) {
        print_av1(bench_av1("scene", desc, scene, frames, preset, av1_target_kbps, true));
        print_av1(bench_av1("ui", desc, ui, frames, preset, av1_target_kbps, true));
    }
    // Single-core baseline (the "16-core and single-core" numbers the plan asks for): same scene,
    // preset 12, parallelism pinned to 1.
    print_av1(bench_av1("scene", desc, scene, frames, 12, av1_target_kbps, false));
    fmt::print("{:-<80}\n", "");
    const double frame_budget_ms = 1000.0 / 30.0;
    fmt::print("(30 fps real-time budget = {:.1f} ms/frame; enc ms above is post-keyframe steady "
               "state, incl. RGBA->YUV.)\n",
               frame_budget_ms);

    fmt::print("\nWAN budget (hosted server, ~100 Mbit uplink): ~10-12 MB/s. LAN: ~100+ MB/s.\n");
    return 0;
}
