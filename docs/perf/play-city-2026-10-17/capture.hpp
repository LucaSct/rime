// External diagnostic only. The production native event/present loop stays intact.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <thread>
#include <string_view>
namespace window_diag {
enum class Phase { Warmup, Hip, SettleAds, Ads, SettleTurn, Turning, Finish };
inline const char* name(Phase p) {
    constexpr const char* names[]{"warmup", "hip", "settle_ads", "ads", "settle_turn", "turning", "finish"};
    return names[static_cast<unsigned>(p)];
}
struct Sample {
    std::uint64_t frame{}, ticks{}, resize_total{}, shots{};
    Phase phase{}, interval_phase{};
    double dt_ms{}, pump_ms{}, sim_ms{}, acquire_ms{}, declare_ms{}, execute_ms{}, present_ms{};
    bool recorded{};
    unsigned width{}, height{}, native_width{}, native_height{}, resize_width{}, resize_height{};
    float aim{}, yaw{}, pitch{};
    unsigned terrain{}, passes{};
    bool feed{}, charge{};
};
class Capture {
public:
    void zone(std::uint64_t frame, std::string_view name, double elapsed) {
        if (frame >= samples_.size()) return;
        auto& s = samples_[frame];
        if (name == "frame.pump") s.pump_ms += elapsed;
        else if (name == "sim.tick") s.sim_ms += elapsed;
        else if (name == "frame.acquire") s.acquire_ms += elapsed;
        else if (name == "frame.declare") s.declare_ms += elapsed;
        else if (name == "frame.execute") s.execute_ms += elapsed;
        else if (name == "frame.present") s.present_ms += elapsed;
    }
    void begin(const rime::app::FrameContext& ctx) {
        phase_ = frames_ < 30 ? Phase::Warmup : frames_ < 150 ? Phase::Hip :
                 frames_ < 180 ? Phase::SettleAds : frames_ < 300 ? Phase::Ads :
                 frames_ < 330 ? Phase::SettleTurn : frames_ < 450 ? Phase::Turning : Phase::Finish;
        for (const auto& e : ctx.input) {
            if (e.type == rime::platform::EventType::WindowResize) {
                ++resizes_;
                resize_width_ = e.resize.size.width;
                resize_height_ = e.resize.size.height;
            }
        }
        if (phase_ == Phase::Turning) turn_seconds_ += ctx.frame_dt;
    }
    bool aiming() const { return phase_ == Phase::SettleAds || phase_ == Phase::Ads; }
    float yaw(float initial) const {
        return initial + (phase_ == Phase::Turning ? .8f * std::sin(float(turn_seconds_) * 1.5f) : 0.0f);
    }
    void record(const rime::app::FrameContext& ctx, std::uint64_t ticks,
                rime::platform::Extent2D native_extent, float aim, float yaw_value, float pitch,
                bool feed, unsigned terrain, unsigned passes, std::uint64_t shots, bool charge) {
        if (ctx.frame_index >= samples_.size()) { frames_ = 451; return; }
        auto& s = samples_[ctx.frame_index];
        s.recorded = true;
        s.frame = ctx.frame_index; s.ticks = ticks - last_ticks_; s.phase = phase_;
        // frame_dt spans the preceding loop iteration, so its phase is the preceding render.
        s.interval_phase = previous_phase_; s.dt_ms = ctx.frame_dt * 1000.0;
        s.width = ctx.extent.width; s.height = ctx.extent.height;
        s.native_width = native_extent.width; s.native_height = native_extent.height;
        s.resize_total = resizes_; s.resize_width = resize_width_; s.resize_height = resize_height_;
        s.aim = aim; s.yaw = yaw_value; s.pitch = pitch; s.feed = feed;
        s.terrain = terrain; s.passes = passes; s.shots = shots; s.charge = charge;
        previous_phase_ = phase_; last_ticks_ = ticks; ++frames_;
    }
    bool done() const { return frames_ >= 451; }
    bool write(const char* path) const {
        if (frames_ != 451) return false;
        for (std::size_t i = 0; i < 451; ++i) if (!samples_[i].recorded) return false;
        std::ofstream out(path);
        if (!out) return false;
        out << "frame,interval_phase,render_phase,dt_ms,ticks,width,height,native_width,native_height,resize_events_total,resize_width,resize_height,aim,yaw,pitch,scope_feed,terrain,declared_passes,shots,charge,pump_ms,sim_ms,acquire_ms,declare_ms,execute_ms,present_ms\n";
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            const auto& s = samples_[i];
            if (!s.recorded) continue;
            out << s.frame << ',' << name(s.interval_phase) << ',' << name(s.phase) << ',' << s.dt_ms
                << ',' << s.ticks << ',' << s.width << ',' << s.height << ',' << s.native_width
                << ',' << s.native_height << ',' << s.resize_total << ',' << s.resize_width
                << ',' << s.resize_height << ',' << s.aim << ',' << s.yaw << ',' << s.pitch
                << ',' << s.feed << ',' << s.terrain << ',' << s.passes << ',' << s.shots
                << ',' << s.charge << ',' << s.pump_ms << ',' << s.sim_ms
                << ',' << s.acquire_ms << ',' << s.declare_ms << ',' << s.execute_ms << ',' << s.present_ms << '\n';
        }
        out.flush();
        return static_cast<bool>(out);
    }
private:
    std::array<Sample, 2048> samples_{};
    std::size_t frames_{};
    std::uint64_t last_ticks_{}, resizes_{};
    unsigned resize_width_{}, resize_height_{};
    Phase phase_{Phase::Warmup}, previous_phase_{Phase::Warmup};
    double turn_seconds_{};
};
} // namespace window_diag
