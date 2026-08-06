#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include <map>
#include <set>

namespace liveac {

struct Sample {
    double time{};
    std::uint64_t command_number{};
    float pitch{};
    float yaw{};
    float forwardmove{};
    float sidemove{};
    std::uint32_t buttons{};
    bool on_ground{};
    bool alive{};

    // Server-side target context supplied by the Metamod collector.
    int target_slot{};
    bool target_visible{};
    bool target_in_crosshair{};
    float target_angle_error{180.0f};
    float previous_target_angle_error{180.0f};
    int weapon_id{};
    float horizontal_speed{};
};


struct DetectorStats {
    std::uint64_t total_samples{};
    std::uint64_t visible_target_samples{};
    std::uint64_t target_acquisitions{};
    std::uint64_t attack_edges{};
    std::uint64_t attacks_on_visible_target{};
    std::uint64_t attacks_in_crosshair{};
    std::uint64_t reaction_samples{};
    std::uint64_t movement_samples{};
    std::uint64_t jump_edges{};
    std::uint64_t valid_landings{};
    std::uint64_t ideal_jumps{};
    std::uint64_t speed_samples{};
    std::uint64_t speed_anomalies{};
    float max_horizontal_speed{};
};

struct Evidence {
    double time{};
    std::string type;
    float weight{};
    std::string details;
    bool detection{}; // false=warning, true=strong detection
};

struct Config {
    // Generic live snap evidence (not an upstream UDS port).
    float snap_min_degrees = 22.0f;
    float snap_max_seconds = 0.040f;
    float snap_weight = 6.0f;
    float attack_after_snap_seconds = 0.080f;
    float snap_attack_weight = 4.0f;
    float target_lock_degrees = 2.5f;
    float target_snap_improvement_degrees = 12.0f;
    int target_snap_required_strikes = 4;

    // UDS AIM TYPE 5 adapted constants/behavior.
    int uds_sensitivity_history = 15;
    float uds_min_sens_detected = 0.0004f;
    float uds_min_sens_warning = 0.002f;
    float uds_min_playable_sens = 0.002f;
    float uds_aim5_attack_window = 0.50f;
    int uds_aim5_warning_strikes = 3;
    float uds_aim5_weight = 10.0f;

    // UDS IDEALJUMP adapted behavior.
    int uds_idealjump_air_frames = 10;
    float uds_idealjump_window_seconds = 0.150f;
    int uds_idealjump_max_strikes = 11;
    float uds_idealjump_weight = 12.0f;

    // UDS autoattack-style command interval sequence (experimental live adaptation).
    int uds_autoattack_min_cmd_gap = 2;
    int uds_autoattack_max_cmd_gap = 7;
    int uds_autoattack_strikes = 4;
    float uds_autoattack_weight = 5.0f;

    // Target acquisition timing. This is deliberately conservative and only
    // emits after a repeated low-variance pattern while a visible enemy is acquired.
    int reaction_min_samples = 8;
    float reaction_fast_ms = 90.0f;
    float reaction_median_ms = 75.0f;
    float reaction_stddev_ms = 18.0f;
    float reaction_weight = 14.0f;

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
    std::size_t detector_diversity() const noexcept { return active_types_.size(); }
    int idealjump_strikes() const noexcept { return idealjump_strikes_; }
    const DetectorStats& stats() const noexcept { return stats_; }
    void reset();

private:
    static float angle_delta(float a, float b);
    void decay(double now);
    bool cooldown_ok(const std::string& type, double now) const;
    void mark(const std::string& type, double now);
    void add(std::vector<Evidence>& out, const Sample& s, const std::string& type,
             float weight, const std::string& details, bool detection);

    Config cfg_;
    std::deque<Sample> history_;
    std::vector<std::pair<std::string, double>> last_events_;
    std::deque<float> angle_step_history_;
    float score_{};
    std::map<std::string, float> type_points_;
    std::set<std::string> active_types_;
    double last_time_{};
    double last_snap_time_{-1000.0};
    double last_tiny_angle_time_{-1000.0};
    bool previous_on_ground_{};
    bool previous_jump_{};
    int airborne_frames_{};
    bool search_next_jump_{};
    int idealjump_strikes_{};
    int aim5_tiny_strikes_{};
    int snap_attack_count_{};
    std::uint64_t last_attack_cmd_{};
    int autoattack_strikes_{};
    int target_snap_strikes_{};
    int last_target_slot_{};
    double target_visible_since_{-1000.0};
    bool previous_target_visible_{};
    std::deque<float> reaction_samples_ms_;
    DetectorStats stats_{};
};

} // namespace liveac
