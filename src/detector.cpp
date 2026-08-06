#include "detector.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace liveac {
namespace {
constexpr std::uint32_t IN_ATTACK = 1u << 0;
constexpr std::uint32_t IN_JUMP   = 1u << 1;
constexpr float EPSILON = 0.0000005f;
}

PlayerDetector::PlayerDetector(Config cfg) : cfg_(cfg) {}

float PlayerDetector::angle_delta(float a, float b) {
    float d = std::fmod(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

void PlayerDetector::decay(double now) {
    if (last_time_ > 0.0 && now > last_time_)
        score_ = std::max(0.0f, score_ - static_cast<float>(now - last_time_) * cfg_.decay_per_second);
    last_time_ = now;
}

bool PlayerDetector::cooldown_ok(const std::string& type, double now) const {
    for (const auto& item : last_events_)
        if (item.first == type) return now - item.second >= cfg_.evidence_cooldown;
    return true;
}

void PlayerDetector::mark(const std::string& type, double now) {
    for (auto& item : last_events_) {
        if (item.first == type) { item.second = now; return; }
    }
    last_events_.push_back({type, now});
}

void PlayerDetector::add(std::vector<Evidence>& out, const Sample& s, const std::string& type,
                         float weight, const std::string& details, bool detection) {
    if (!cooldown_ok(type, s.time)) return;
    score_ = std::min(100.0f, score_ + weight);
    out.push_back({s.time, type, weight, details, detection});
    mark(type, s.time);
}

std::vector<Evidence> PlayerDetector::push(const Sample& s) {
    std::vector<Evidence> out;
    decay(s.time);
    if (!s.alive) {
        history_.clear(); angle_step_history_.clear(); airborne_frames_ = 0;
        search_next_jump_ = false; idealjump_strikes_ = 0; aim5_tiny_strikes_ = 0;
        previous_on_ground_ = s.on_ground; previous_jump_ = false;
        return out;
    }

    const bool attack_now = (s.buttons & IN_ATTACK) != 0;
    const bool jump_now = (s.buttons & IN_JUMP) != 0;

    if (!history_.empty()) {
        const auto& p = history_.back();
        const double dt = s.time - p.time;
        const float dx = std::fabs(angle_delta(s.pitch, p.pitch));
        const float dy = std::fabs(angle_delta(s.yaw, p.yaw));
        const float distance = std::sqrt(dx * dx + dy * dy);
        const bool attack_edge = attack_now && ((p.buttons & IN_ATTACK) == 0);

        // Generic live snap detector retained as supporting evidence.
        if (dt > 0.0 && dt <= cfg_.snap_max_seconds && distance >= cfg_.snap_min_degrees) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << "angle=" << distance
               << "deg dt=" << (dt * 1000.0) << "ms";
            add(out, s, "LIVE_AIM_SNAP", cfg_.snap_weight, ss.str(), false);
            last_snap_time_ = s.time;
        }
        if (attack_edge && s.time - last_snap_time_ <= cfg_.attack_after_snap_seconds) {
            if (++snap_attack_count_ >= 3) {
                add(out, s, "LIVE_SNAP_ATTACK_PATTERN", cfg_.snap_attack_weight,
                    "three attacks occurred immediately after large angle corrections", false);
                snap_attack_count_ = 0;
            }
        }

        // UDS AIM TYPE 5 adaptation: learn the player's minimum normal angle step from
        // 15+ samples, then flag non-zero sub-threshold angle movement near an attack.
        const float step = std::min(dx > EPSILON ? dx : 9999.0f, dy > EPSILON ? dy : 9999.0f);
        if (step < 9999.0f) {
            float baseline = cfg_.uds_min_playable_sens;
            if (static_cast<int>(angle_step_history_.size()) >= cfg_.uds_sensitivity_history) {
                baseline = *std::min_element(angle_step_history_.end() - cfg_.uds_sensitivity_history,
                                             angle_step_history_.end());
                baseline = std::max(baseline, cfg_.uds_min_playable_sens);
            }
            const float real_threshold = std::max(cfg_.uds_min_sens_detected, baseline / 2.1f);
            const float warn_threshold = std::max(cfg_.uds_min_sens_warning, baseline / 1.001f);
            if (step < real_threshold) {
                last_tiny_angle_time_ = s.time;
                ++aim5_tiny_strikes_;
            } else if (step < warn_threshold) {
                last_tiny_angle_time_ = s.time;
                aim5_tiny_strikes_ = std::max(1, aim5_tiny_strikes_);
            }
            angle_step_history_.push_back(step);
            while (angle_step_history_.size() > 64) angle_step_history_.pop_front();
        }
        if (attack_edge && s.time - last_tiny_angle_time_ <= cfg_.uds_aim5_attack_window &&
            aim5_tiny_strikes_ >= cfg_.uds_aim5_warning_strikes) {
            std::ostringstream ss;
            ss << "UDS AIM TYPE 5 adapted: sub-baseline angle steps=" << aim5_tiny_strikes_
               << " within " << static_cast<int>(cfg_.uds_aim5_attack_window * 1000.0f) << "ms of attack";
            add(out, s, "UDS_AIM_TYPE_5_ADAPTED", cfg_.uds_aim5_weight, ss.str(), true);
            aim5_tiny_strikes_ = 0;
        }

        // UDS autoattack-style command gap detector. The original also uses weapon
        // availability frames; live v0.2 records this as experimental evidence only.
        if (attack_edge) {
            if (last_attack_cmd_ > 0 && s.command_number > last_attack_cmd_) {
                const auto gap = static_cast<int>(s.command_number - last_attack_cmd_);
                if (gap >= cfg_.uds_autoattack_min_cmd_gap && gap <= cfg_.uds_autoattack_max_cmd_gap)
                    ++autoattack_strikes_;
                else
                    autoattack_strikes_ = 0;
                if (autoattack_strikes_ >= cfg_.uds_autoattack_strikes) {
                    add(out, s, "UDS_AUTOATTACK_ADAPTED", cfg_.uds_autoattack_weight,
                        "repeated attack edges with command gaps 2..7 (weapon timing unavailable)", false);
                    autoattack_strikes_ = 0;
                }
            }
            last_attack_cmd_ = s.command_number;
        }
    }

    // UDS IDEALJUMP adaptation: after >10 airborne frames, arm on landing and require
    // the next takeoff/jump transition within 150 ms. Emit after >11 consecutive strikes.
    if (!s.on_ground) ++airborne_frames_;
    const bool landed = s.on_ground && !previous_on_ground_;
    const bool took_off = !s.on_ground && previous_on_ground_;
    if (landed) {
        search_next_jump_ = airborne_frames_ > cfg_.uds_idealjump_air_frames;
        airborne_frames_ = 0;
    }
    if (search_next_jump_ && took_off) {
        search_next_jump_ = false;
        const bool fresh_jump_edge = jump_now && !previous_jump_;
        const double since_landing = history_.empty() ? 999.0 : s.time - history_.back().time;
        if (fresh_jump_edge && since_landing <= cfg_.uds_idealjump_window_seconds) {
            ++idealjump_strikes_;
            if (idealjump_strikes_ > cfg_.uds_idealjump_max_strikes) {
                add(out, s, "UDS_IDEALJUMP_ADAPTED", cfg_.uds_idealjump_weight,
                    "more than 11 consecutive ideal jumps after >10 airborne frames", true);
                idealjump_strikes_ = 0;
            }
        } else {
            idealjump_strikes_ = 0;
        }
    }

    previous_on_ground_ = s.on_ground;
    previous_jump_ = jump_now;
    history_.push_back(s);
    while (!history_.empty() && s.time - history_.front().time > 12.0) history_.pop_front();
    return out;
}

void PlayerDetector::reset() {
    history_.clear(); last_events_.clear(); angle_step_history_.clear(); score_ = 0.0f;
    last_time_ = 0.0; last_snap_time_ = -1000.0; last_tiny_angle_time_ = -1000.0;
    previous_on_ground_ = false; previous_jump_ = false; airborne_frames_ = 0;
    search_next_jump_ = false; idealjump_strikes_ = 0; aim5_tiny_strikes_ = 0;
    snap_attack_count_ = 0; last_attack_cmd_ = 0; autoattack_strikes_ = 0;
}
} // namespace liveac
