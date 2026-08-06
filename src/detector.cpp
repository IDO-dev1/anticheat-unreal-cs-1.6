#include "detector.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace liveac {
namespace {
constexpr std::uint32_t IN_ATTACK = 1u << 0;
constexpr std::uint32_t IN_JUMP   = 1u << 1;
}

PlayerDetector::PlayerDetector(Config cfg) : cfg_(cfg) {}

float PlayerDetector::angle_delta(float a, float b) {
    float d = std::fmod(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

void PlayerDetector::decay(double now) {
    if (last_time_ > 0.0 && now > last_time_) {
        score_ = std::max(0.0f, score_ - static_cast<float>(now - last_time_) * cfg_.decay_per_second);
    }
    last_time_ = now;
}

bool PlayerDetector::cooldown_ok(const std::string& type, double now) const {
    for (const auto& item : last_events_) {
        if (item.first == type) return now - item.second >= cfg_.evidence_cooldown;
    }
    return true;
}

void PlayerDetector::mark(const std::string& type, double now) {
    for (auto& item : last_events_) {
        if (item.first == type) { item.second = now; return; }
    }
    last_events_.push_back({type, now});
}

std::vector<Evidence> PlayerDetector::push(const Sample& s) {
    std::vector<Evidence> out;
    decay(s.time);
    if (!s.alive) { history_.clear(); bhop_chain_ = 0; previous_on_ground_ = s.on_ground; return out; }

    if (!history_.empty()) {
        const auto& p = history_.back();
        const double dt = s.time - p.time;
        const float dy = angle_delta(s.yaw, p.yaw);
        const float dp = angle_delta(s.pitch, p.pitch);
        const float distance = std::sqrt(dy * dy + dp * dp);

        if (dt > 0.0 && dt <= cfg_.snap_max_seconds && distance >= cfg_.snap_min_degrees && cooldown_ok("AIM_SNAP", s.time)) {
            score_ += cfg_.snap_weight;
            last_snap_time_ = s.time;
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << "angle=" << distance << "deg dt=" << (dt * 1000.0) << "ms";
            out.push_back({s.time, "AIM_SNAP", cfg_.snap_weight, ss.str()});
            mark("AIM_SNAP", s.time);
        }

        const bool attacking_now = (s.buttons & IN_ATTACK) != 0;
        const bool attacked_before = (p.buttons & IN_ATTACK) != 0;
        if (attacking_now && !attacked_before && s.time - last_snap_time_ <= cfg_.attack_after_snap_seconds) {
            ++fast_attack_after_snap_count_;
            if (fast_attack_after_snap_count_ >= 3 && cooldown_ok("SNAP_ATTACK_PATTERN", s.time)) {
                score_ += cfg_.trigger_repeat_weight;
                out.push_back({s.time, "SNAP_ATTACK_PATTERN", cfg_.trigger_repeat_weight,
                    "repeated attack immediately after large angle correction"});
                mark("SNAP_ATTACK_PATTERN", s.time);
                fast_attack_after_snap_count_ = 0;
            }
        }
    }

    const bool jump_now = (s.buttons & IN_JUMP) != 0;
    const bool landed = s.on_ground && !previous_on_ground_;
    if (landed) {
        if (jump_now && !previous_jump_) ++bhop_chain_;
        else bhop_chain_ = 0;
        if (bhop_chain_ >= cfg_.bhop_min_chain && cooldown_ok("IDEAL_BHOP", s.time)) {
            score_ += cfg_.bhop_weight;
            out.push_back({s.time, "IDEAL_BHOP", cfg_.bhop_weight,
                "jump pressed on landing for " + std::to_string(bhop_chain_) + " consecutive landings"});
            mark("IDEAL_BHOP", s.time);
            bhop_chain_ = 0;
        }
    }
    previous_on_ground_ = s.on_ground;
    previous_jump_ = jump_now;

    history_.push_back(s);
    while (!history_.empty() && s.time - history_.front().time > 12.0) history_.pop_front();
    score_ = std::min(score_, 100.0f);
    return out;
}

void PlayerDetector::reset() {
    history_.clear(); last_events_.clear(); score_ = 0.0f; last_time_ = 0.0;
    last_snap_time_ = -1000.0; previous_on_ground_ = false; previous_jump_ = false;
    bhop_chain_ = 0; fast_attack_after_snap_count_ = 0;
}
} // namespace liveac
