#include "detector.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#ifndef IN_ATTACK
#define IN_ATTACK (1u << 0)
#endif
#ifndef IN_JUMP
#define IN_JUMP (1u << 1)
#endif

namespace liveac {
namespace {
constexpr float EPSILON = 0.000001f;
float median(std::deque<float> values) {
    if (values.empty()) return 9999.0f;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 ? values[n/2] : (values[n/2-1] + values[n/2]) * 0.5f;
}
float stddev(const std::deque<float>& values) {
    if (values.size() < 2) return 9999.0f;
    const float mean = std::accumulate(values.begin(), values.end(), 0.0f) / values.size();
    float sum = 0.0f;
    for (float v : values) { const float d = v - mean; sum += d*d; }
    return std::sqrt(sum / values.size());
}
}

PlayerDetector::PlayerDetector(Config cfg) : cfg_(std::move(cfg)) {}

float PlayerDetector::angle_delta(float a, float b) {
    float d = std::fmod(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

void PlayerDetector::decay(double now) {
    if (last_time_ > 0.0 && now > last_time_) {
        const float decay = static_cast<float>((now - last_time_) * cfg_.decay_per_second);
        for (auto it = type_points_.begin(); it != type_points_.end();) {
            it->second = std::max(0.0f, it->second - decay);
            if (it->second <= 0.001f) { active_types_.erase(it->first); it = type_points_.erase(it); }
            else ++it;
        }
        float total = 0.0f;
        for (const auto& item : type_points_) total += item.second;
        const float cap = active_types_.size() <= 1 ? 35.0f : (active_types_.size() == 2 ? 65.0f : 100.0f);
        score_ = std::min(cap, total);
    }
    last_time_ = now;
}

bool PlayerDetector::cooldown_ok(const std::string& type, double now) const {
    for (const auto& e : last_events_) if (e.first == type && now - e.second < cfg_.evidence_cooldown) return false;
    return true;
}
void PlayerDetector::mark(const std::string& type, double now) {
    for (auto& e : last_events_) if (e.first == type) { e.second = now; return; }
    last_events_.push_back({type, now});
}
void PlayerDetector::add(std::vector<Evidence>& out, const Sample& s, const std::string& type,
                         float weight, const std::string& details, bool detection) {
    if (!cooldown_ok(type, s.time)) return;
    float cap = 16.0f;
    if (type == "UDS_IDEALJUMP_ADAPTED") cap = 30.0f;
    else if (type == "TARGET_AIM_SNAP") cap = 18.0f;
    else if (type == "TARGET_REACTION_PATTERN") cap = 24.0f;
    else if (type == "UDS_AIM_TYPE_5_CONTEXT") cap = 18.0f;
    else if (type == "UDS_AUTOATTACK_CONTEXT") cap = 10.0f;

    const float before = type_points_[type];
    const float after = std::min(cap, before + weight);
    const float accepted = after - before;
    if (accepted <= 0.0f) return;
    type_points_[type] = after;
    active_types_.insert(type);
    float total = 0.0f;
    for (const auto& item : type_points_) total += item.second;
    const float diversity_cap = active_types_.size() <= 1 ? 35.0f : (active_types_.size() == 2 ? 65.0f : 100.0f);
    score_ = std::min(diversity_cap, total);
    out.push_back({s.time, type, accepted, details, detection});
    mark(type, s.time);
}

std::vector<Evidence> PlayerDetector::push(const Sample& s) {
    std::vector<Evidence> out;
    decay(s.time);
    if (!s.alive) {
        history_.clear(); angle_step_history_.clear(); reaction_samples_ms_.clear(); airborne_frames_=0;
        search_next_jump_=false; idealjump_strikes_=0; aim5_tiny_strikes_=0; target_snap_strikes_=0;
        previous_on_ground_=s.on_ground; previous_jump_=false; previous_target_visible_=false;
        return out;
    }

    ++stats_.total_samples;
    ++stats_.movement_samples;
    ++stats_.speed_samples;
    stats_.max_horizontal_speed = std::max(stats_.max_horizontal_speed, s.horizontal_speed);
    // Conservative reporting-only threshold. This does not add suspicion points.
    if (s.horizontal_speed > 520.0f) ++stats_.speed_anomalies;

    const bool attack_now = (s.buttons & IN_ATTACK) != 0;
    const bool jump_now = (s.buttons & IN_JUMP) != 0;
    if (s.target_visible) ++stats_.visible_target_samples;

    // Track first visibility/acquisition of a target for reaction-time evidence.
    if (s.target_visible && (!previous_target_visible_ || s.target_slot != last_target_slot_)) {
        target_visible_since_ = s.time;
        last_target_slot_ = s.target_slot;
        ++stats_.target_acquisitions;
    }

    if (!history_.empty()) {
        const auto& p = history_.back();
        const double dt = s.time - p.time;
        const float dx = std::fabs(angle_delta(s.pitch, p.pitch));
        const float dy = std::fabs(angle_delta(s.yaw, p.yaw));
        const float distance = std::sqrt(dx*dx + dy*dy);
        const bool attack_edge = attack_now && ((p.buttons & IN_ATTACK) == 0);
        if (attack_edge) {
            ++stats_.attack_edges;
            if (s.target_visible) ++stats_.attacks_on_visible_target;
            if (s.target_in_crosshair) ++stats_.attacks_in_crosshair;
        }
        const bool jump_edge = jump_now && ((p.buttons & IN_JUMP) == 0);
        if (jump_edge) ++stats_.jump_edges;

        // Context-aware snap: must be fast, end near a visible enemy and improve aim materially.
        const float improvement = s.previous_target_angle_error - s.target_angle_error;
        if (dt > 0.0 && dt <= cfg_.snap_max_seconds && distance >= cfg_.snap_min_degrees &&
            s.target_visible && s.target_angle_error <= cfg_.target_lock_degrees &&
            improvement >= cfg_.target_snap_improvement_degrees) {
            last_snap_time_ = s.time;
            ++target_snap_strikes_;
        } else if (!s.target_visible) {
            target_snap_strikes_ = std::max(0, target_snap_strikes_ - 1);
        }
        if (attack_edge && s.target_visible && s.target_angle_error <= cfg_.target_lock_degrees &&
            s.time - last_snap_time_ <= cfg_.attack_after_snap_seconds &&
            target_snap_strikes_ >= cfg_.target_snap_required_strikes) {
            std::ostringstream ss;
            ss << "repeated fast corrections ended " << s.target_angle_error
               << "deg from visible target; improvement=" << improvement << "deg";
            add(out, s, "TARGET_AIM_SNAP", cfg_.snap_attack_weight, ss.str(), false);
            target_snap_strikes_ = 0;
        }

        // Aim Type 5 adaptation is now accepted only in target context.
        const float step = std::min(dx > EPSILON ? dx : 9999.0f, dy > EPSILON ? dy : 9999.0f);
        if (step < 9999.0f) {
            float baseline = cfg_.uds_min_playable_sens;
            if (static_cast<int>(angle_step_history_.size()) >= cfg_.uds_sensitivity_history) {
                baseline = *std::min_element(angle_step_history_.end()-cfg_.uds_sensitivity_history,
                                             angle_step_history_.end());
                baseline = std::max(baseline, cfg_.uds_min_playable_sens);
            }
            const float real_threshold = std::max(cfg_.uds_min_sens_detected, baseline/2.1f);
            const float warn_threshold = std::max(cfg_.uds_min_sens_warning, baseline/1.001f);
            if (s.target_visible && s.target_angle_error <= cfg_.target_lock_degrees) {
                if (step < real_threshold) { last_tiny_angle_time_=s.time; ++aim5_tiny_strikes_; }
                else if (step < warn_threshold) { last_tiny_angle_time_=s.time; aim5_tiny_strikes_=std::max(1,aim5_tiny_strikes_); }
            }
            angle_step_history_.push_back(step);
            while (angle_step_history_.size()>64) angle_step_history_.pop_front();
        }
        if (attack_edge && s.target_visible && s.target_angle_error <= cfg_.target_lock_degrees &&
            s.time-last_tiny_angle_time_ <= cfg_.uds_aim5_attack_window &&
            aim5_tiny_strikes_ >= cfg_.uds_aim5_warning_strikes) {
            std::ostringstream ss; ss << "sub-baseline micro-steps=" << aim5_tiny_strikes_
                << " while locked to visible target #" << s.target_slot;
            add(out,s,"UDS_AIM_TYPE_5_CONTEXT",cfg_.uds_aim5_weight,ss.str(),true);
            aim5_tiny_strikes_=0;
        }

        // Autoattack evidence requires an actual target under the crosshair.
        if (attack_edge && s.target_in_crosshair) {
            if (last_attack_cmd_>0 && s.command_number>last_attack_cmd_) {
                const int gap=static_cast<int>(s.command_number-last_attack_cmd_);
                if (gap>=cfg_.uds_autoattack_min_cmd_gap && gap<=cfg_.uds_autoattack_max_cmd_gap) ++autoattack_strikes_;
                else autoattack_strikes_=0;
                if (autoattack_strikes_>=cfg_.uds_autoattack_strikes) {
                    add(out,s,"UDS_AUTOATTACK_CONTEXT",cfg_.uds_autoattack_weight,
                        "repeated attack-edge command gaps while crosshair intersected an enemy",false);
                    autoattack_strikes_=0;
                }
            }
            last_attack_cmd_=s.command_number;
        } else if (attack_edge) autoattack_strikes_=0;

        // Repeated fast and stable acquisition timing; never generated from one event.
        if (attack_edge && s.target_visible && s.target_angle_error <= cfg_.target_lock_degrees && target_visible_since_>0.0) {
            const float reaction=static_cast<float>((s.time-target_visible_since_)*1000.0);
            if (reaction>=0.0f && reaction<=500.0f) {
                reaction_samples_ms_.push_back(reaction);
                ++stats_.reaction_samples;
                while (reaction_samples_ms_.size()>16) reaction_samples_ms_.pop_front();
                if (static_cast<int>(reaction_samples_ms_.size())>=cfg_.reaction_min_samples) {
                    const float med=median(reaction_samples_ms_); const float sd=stddev(reaction_samples_ms_);
                    const int fast=static_cast<int>(std::count_if(reaction_samples_ms_.begin(),reaction_samples_ms_.end(),
                        [&](float v){return v<=cfg_.reaction_fast_ms;}));
                    if (med<=cfg_.reaction_median_ms && sd<=cfg_.reaction_stddev_ms && fast>=cfg_.reaction_min_samples-1) {
                        std::ostringstream ss; ss << "reaction median=" << med << "ms stddev=" << sd
                            << "ms fast=" << fast << "/" << reaction_samples_ms_.size();
                        add(out,s,"TARGET_REACTION_PATTERN",cfg_.reaction_weight,ss.str(),true);
                        reaction_samples_ms_.clear();
                    }
                }
            }
        }
    }

    // UDS IDEALJUMP adaptation, unchanged and independent of aim context.
    if (!s.on_ground) ++airborne_frames_;
    const bool landed=s.on_ground && !previous_on_ground_;
    const bool took_off=!s.on_ground && previous_on_ground_;
    if (landed) { ++stats_.valid_landings; search_next_jump_=airborne_frames_>cfg_.uds_idealjump_air_frames; airborne_frames_=0; }
    if (search_next_jump_ && took_off) {
        search_next_jump_=false;
        const bool edge=jump_now && !previous_jump_;
        const double since=history_.empty()?999.0:s.time-history_.back().time;
        if (edge && since<=cfg_.uds_idealjump_window_seconds) {
            ++idealjump_strikes_;
            ++stats_.ideal_jumps;
            if (idealjump_strikes_>cfg_.uds_idealjump_max_strikes) {
                add(out,s,"UDS_IDEALJUMP_ADAPTED",cfg_.uds_idealjump_weight,
                    "more than 11 consecutive ideal jumps after >10 airborne frames",true);
                idealjump_strikes_=0;
            }
        } else idealjump_strikes_=0;
    }

    previous_on_ground_=s.on_ground; previous_jump_=jump_now;
    previous_target_visible_=s.target_visible; last_target_slot_=s.target_slot;
    history_.push_back(s);
    while (!history_.empty() && s.time-history_.front().time>12.0) history_.pop_front();
    return out;
}

void PlayerDetector::reset() {
    history_.clear(); last_events_.clear(); angle_step_history_.clear(); reaction_samples_ms_.clear();
    type_points_.clear(); active_types_.clear(); score_=0.0f; last_time_=0.0; last_snap_time_=-1000.0;
    last_tiny_angle_time_=-1000.0; previous_on_ground_=false; previous_jump_=false; airborne_frames_=0;
    search_next_jump_=false; idealjump_strikes_=0; aim5_tiny_strikes_=0; snap_attack_count_=0;
    last_attack_cmd_=0; autoattack_strikes_=0; target_snap_strikes_=0; last_target_slot_=0;
    target_visible_since_=-1000.0; previous_target_visible_=false; stats_ = {};
}
} // namespace liveac
