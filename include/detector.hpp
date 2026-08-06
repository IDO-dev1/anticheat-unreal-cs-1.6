#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace liveac {

struct Sample {
    double time{};
    float pitch{};
    float yaw{};
    float forwardmove{};
    float sidemove{};
    std::uint32_t buttons{};
    bool on_ground{};
    bool alive{};
};

struct Evidence {
    double time{};
    std::string type;
    float weight{};
    std::string details;
};

struct Config {
    float snap_min_degrees = 22.0f;
    float snap_max_seconds = 0.040f;
    float snap_weight = 8.0f;
    float attack_after_snap_seconds = 0.080f;
    float trigger_repeat_weight = 5.0f;
    int bhop_min_chain = 8;
    float bhop_weight = 8.0f;
    float alert_score = 55.0f;
    float high_score = 75.0f;
    float decay_per_second = 0.20f;
    float evidence_cooldown = 0.75f;
};

class PlayerDetector {
public:
    explicit PlayerDetector(Config cfg = {});
    std::vector<Evidence> push(const Sample& sample);
    float score() const noexcept { return score_; }
    int bhop_chain() const noexcept { return bhop_chain_; }
    void reset();

private:
    static float angle_delta(float a, float b);
    void decay(double now);
    bool cooldown_ok(const std::string& type, double now) const;
    void mark(const std::string& type, double now);

    Config cfg_;
    std::deque<Sample> history_;
    std::vector<std::pair<std::string, double>> last_events_;
    float score_{};
    double last_time_{};
    double last_snap_time_{-1000.0};
    bool previous_on_ground_{};
    bool previous_jump_{};
    int bhop_chain_{};
    int fast_attack_after_snap_count_{};
};

} // namespace liveac
