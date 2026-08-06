// Standard C++ headers must be loaded before legacy HLSDK headers.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <cerrno>
#include <ctime>
#include <iomanip>
#include <sys/stat.h>
#include <sys/types.h>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "detector.hpp"

#include <extdll.h>
#include <usercmd.h>
#include <meta_api.h>
#include <dllapi.h>
#include <engine_api.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "Live Unreal Scanner",
    "1.0.1",
    "2026-08-06",
    "Behavioral anti-cheat developed by IDO",
    "https://github.com/",
    "LIVEAC",
    PT_ANYTIME,
    PT_ANYPAUSE
};

enginefuncs_t g_engfuncs;
globalvars_t* gpGlobals = nullptr;
meta_globals_t* gpMetaGlobals = nullptr;
gamedll_funcs_t* gpGamedllFuncs = nullptr;
mutil_funcs_t* gpMetaUtilFuncs = nullptr;

namespace {
constexpr int MAX_CLIENTS_LOCAL = 32;
constexpr int DEFAULT_SCAN_SECONDS = 60;
constexpr int MIN_SCAN_SECONDS = 15;
constexpr int MAX_SCAN_SECONDS = 300;

std::array<std::unique_ptr<liveac::PlayerDetector>, MAX_CLIENTS_LOCAL + 1> detectors;
std::array<float, MAX_CLIENTS_LOCAL + 1> last_alert{};
std::array<std::uint64_t, MAX_CLIENTS_LOCAL + 1> cmd_sequence{};
std::array<float, MAX_CLIENTS_LOCAL + 1> previous_target_error{};
std::array<int, MAX_CLIENTS_LOCAL + 1> current_weapon{};
std::array<unsigned int, MAX_CLIENTS_LOCAL + 1> evidence_count{};
std::array<std::string, MAX_CLIENTS_LOCAL + 1> last_evidence_type{};
std::array<double, MAX_CLIENTS_LOCAL + 1> last_evidence_time{};
std::array<std::map<std::string, unsigned int>, MAX_CLIENTS_LOCAL + 1> evidence_types;

std::array<bool, MAX_CLIENTS_LOCAL + 1> scan_active{};
std::array<double, MAX_CLIENTS_LOCAL + 1> scan_started{};
std::array<double, MAX_CLIENTS_LOCAL + 1> scan_ends{};
std::array<float, MAX_CLIENTS_LOCAL + 1> scan_start_score{};
std::array<unsigned int, MAX_CLIENTS_LOCAL + 1> scan_start_evidence{};
std::array<std::map<std::string, unsigned int>, MAX_CLIENTS_LOCAL + 1> scan_start_types;
std::array<liveac::DetectorStats, MAX_CLIENTS_LOCAL + 1> scan_start_stats{};
std::array<int, MAX_CLIENTS_LOCAL + 1> scan_requester{};

std::array<bool, MAX_CLIENTS_LOCAL + 1> watch_active{};
std::array<int, MAX_CLIENTS_LOCAL + 1> watch_requester{};

std::set<std::string> admin_steamids;
std::string admins_file_path;
liveac::Config config;
std::string liveac_base_dir;
std::string liveac_log_dir;
std::string config_file_path;
constexpr std::size_t LOG_ROTATE_BYTES = 25u * 1024u * 1024u;

int player_index(const edict_t* ent) { return ENTINDEX(const_cast<edict_t*>(ent)); }

bool active_player(int id) {
    if (id < 1 || id > MAX_CLIENTS_LOCAL) return false;
    edict_t* ent = INDEXENT(id);
    return ent && !ent->free && ent->pvPrivateData;
}

bool is_bot(int id) {
    if (!active_player(id)) return false;
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    return auth && std::strcmp(auth, "BOT") == 0;
}

std::string player_name(int id);
void reset_player(int id);

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}


std::string lowercase(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::string text_escape(std::string input) {
    for (char& c : input) {
        if (c == '\n' || c == '\r' || c == '\t' || static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    return input;
}

bool ensure_directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    return false;
}

void rotate_if_needed(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || static_cast<std::size_t>(st.st_size) < LOG_ROTATE_BYTES) return;
    const std::string backup = path + ".1";
    std::remove(backup.c_str());
    std::rename(path.c_str(), backup.c_str());
}

std::string auth_id(int id) {
    if (!active_player(id)) return "unknown";
    const char* raw = GETPLAYERAUTHID(INDEXENT(id));
    return raw ? raw : "unknown";
}

void audit_action(edict_t* caller, const std::string& action, const std::string& target = "") {
    if (liveac_log_dir.empty()) return;
    rotate_if_needed(liveac_log_dir + "/admin_audit.log");
    std::ofstream file(liveac_log_dir + "/admin_audit.log", std::ios::app);
    if (!file) return;
    const int slot = caller ? player_index(caller) : 0;
    const std::string name = caller ? text_escape(player_name(slot)) : "SERVER/RCON";
    const std::string auth = caller ? auth_id(slot) : "SERVER";
    file << "time=" << (gpGlobals ? gpGlobals->time : 0.0)
         << " admin=\"" << name << "\" auth=\"" << text_escape(auth)
         << "\" action=\"" << text_escape(action) << "\" target=\""
         << text_escape(target) << "\"\n";
}

bool parse_bool(const std::string& value, bool& out) {
    const std::string v = lowercase(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
    return false;
}

template <typename T>
bool parse_number(const std::string& value, T& out) {
    std::istringstream in(value);
    T parsed{};
    in >> parsed;
    if (!in || !in.eof()) return false;
    out = parsed;
    return true;
}

bool parse_bool(const std::string& value, bool& out) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value)
        normalized.push_back(static_cast<char>(std::tolower(ch)));

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        out = false;
        return true;
    }
    return false;
}

bool apply_config_value(liveac::Config& cfg, const std::string& key, const std::string& value) {
#define LIVEAC_FLOAT(name) if (key == #name) return parse_number(value, cfg.name)
#define LIVEAC_INT(name) if (key == #name) return parse_number(value, cfg.name)
    LIVEAC_FLOAT(snap_min_degrees); LIVEAC_FLOAT(snap_max_seconds); LIVEAC_FLOAT(snap_weight);
    LIVEAC_FLOAT(attack_after_snap_seconds); LIVEAC_FLOAT(snap_attack_weight);
    LIVEAC_FLOAT(target_lock_degrees); LIVEAC_FLOAT(target_snap_improvement_degrees);
    LIVEAC_INT(target_snap_required_strikes); LIVEAC_INT(uds_sensitivity_history);
    LIVEAC_FLOAT(uds_min_sens_detected); LIVEAC_FLOAT(uds_min_sens_warning);
    LIVEAC_FLOAT(uds_min_playable_sens); LIVEAC_FLOAT(uds_aim5_attack_window);
    LIVEAC_INT(uds_aim5_warning_strikes); LIVEAC_FLOAT(uds_aim5_weight);
    LIVEAC_INT(uds_idealjump_air_frames); LIVEAC_FLOAT(uds_idealjump_window_seconds);
    LIVEAC_INT(uds_idealjump_max_strikes); LIVEAC_FLOAT(uds_idealjump_weight);
    LIVEAC_INT(uds_autoattack_min_cmd_gap); LIVEAC_INT(uds_autoattack_max_cmd_gap);
    LIVEAC_INT(uds_autoattack_strikes); LIVEAC_FLOAT(uds_autoattack_weight);
    LIVEAC_INT(reaction_min_samples); LIVEAC_FLOAT(reaction_fast_ms);
    LIVEAC_FLOAT(reaction_median_ms); LIVEAC_FLOAT(reaction_stddev_ms);
    LIVEAC_FLOAT(reaction_weight); LIVEAC_FLOAT(alert_score); LIVEAC_FLOAT(high_score);
    LIVEAC_FLOAT(decay_per_second); LIVEAC_FLOAT(evidence_cooldown);
    if (key == "liveac_allow_bot_targets") return parse_bool(value, cfg.liveac_allow_bot_targets);
    if (key == "liveac_allow_bot_scan") return parse_bool(value, cfg.liveac_allow_bot_scan);
#undef LIVEAC_FLOAT
#undef LIVEAC_INT
    return false;
}

bool load_config() {
    liveac::Config loaded{};
    std::ifstream file(config_file_path);
    if (!file) {
        SERVER_PRINT("[LiveAC][CFG] liveac.cfg not found; using compiled defaults.\n");
        config = loaded;
        return false;
    }
    std::string line;
    unsigned int line_no = 0, loaded_count = 0;
    while (std::getline(file, line)) {
        ++line_no;
        const auto comment = line.find_first_of(";#");
        const auto slash_comment = line.find("//");
        std::size_t cut = std::string::npos;
        if (comment != std::string::npos) cut = comment;
        if (slash_comment != std::string::npos) cut = std::min(cut, slash_comment);
        if (cut != std::string::npos) line.erase(cut);
        line = trim(line);
        if (line.empty()) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            char message[256];
            std::snprintf(message, sizeof(message), "[LiveAC][CFG] line %u: expected key=value.\n", line_no);
            SERVER_PRINT(message);
            continue;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (!apply_config_value(loaded, key, value)) {
            char message[320];
            std::snprintf(message, sizeof(message), "[LiveAC][CFG] line %u: unknown key or invalid value '%s'.\n", line_no, key.c_str());
            SERVER_PRINT(message);
            continue;
        }
        ++loaded_count;
    }
    config = loaded;
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) if (detectors[id]) reset_player(id);
    char message[256];
    std::snprintf(message, sizeof(message), "[LiveAC][CFG] Loaded %u setting(s). Active detectors reset.\n", loaded_count);
    SERVER_PRINT(message);
    return true;
}

void print_to(edict_t* receiver, const std::string& text) {
    if (receiver && !receiver->free)
        g_engfuncs.pfnClientPrintf(receiver, print_console, text.c_str());
    else
        SERVER_PRINT(text.c_str());
}

void print_to_slot(int slot, const std::string& text) {
    if (active_player(slot)) print_to(INDEXENT(slot), text);
    else print_to(nullptr, text);
}

bool load_admins() {
    admin_steamids.clear();
    char game_dir[256]{};
    GET_GAME_DIR(game_dir);
    admins_file_path = std::string(game_dir) + "/addons/liveac/liveac_admins.ini";

    std::ifstream file(admins_file_path);
    if (!file) {
        SERVER_PRINT("[LiveAC] Warning: addons/liveac/liveac_admins.ini not found; in-game commands disabled.\n");
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        const auto comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (!line.empty()) admin_steamids.insert(line);
    }

    char message[256];
    std::snprintf(message, sizeof(message), "[LiveAC] Loaded %u in-game admin SteamID(s).\n",
                  static_cast<unsigned int>(admin_steamids.size()));
    SERVER_PRINT(message);
    return true;
}

bool is_authorized(edict_t* caller) {
    if (!caller) return true; // Server console and RCON.
    const char* auth = GETPLAYERAUTHID(caller);
    return auth && admin_steamids.find(auth) != admin_steamids.end();
}

bool require_admin(edict_t* caller) {
    if (is_authorized(caller)) return true;
    print_to(caller, "[LiveAC] Access denied. Add your SteamID to addons/liveac/liveac_admins.ini\n");
    return false;
}

void ensure_player(int id) {
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL && !detectors[id])
        detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
}

void clear_scan(int id) {
    scan_active[id] = false;
    scan_started[id] = 0.0;
    scan_ends[id] = 0.0;
    scan_start_score[id] = 0.0f;
    scan_start_evidence[id] = 0;
    scan_start_types[id].clear();
    scan_start_stats[id] = {};
    scan_requester[id] = 0;
}

void reset_player(int id) {
    if (id < 1 || id > MAX_CLIENTS_LOCAL) return;
    detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
    last_alert[id] = 0.0f;
    cmd_sequence[id] = 0;
    previous_target_error[id] = 180.0f;
    current_weapon[id] = 0;
    evidence_count[id] = 0;
    last_evidence_type[id].clear();
    last_evidence_time[id] = 0.0;
    evidence_types[id].clear();
    clear_scan(id);
    watch_active[id] = false;
    watch_requester[id] = 0;
}

std::string player_name(int id) {
    if (!active_player(id)) return "unknown";
    const char* name = STRING(INDEXENT(id)->v.netname);
    return name ? name : "unknown";
}

void log_evidence(int id, const liveac::Evidence& ev, float score) {
    const std::string auth = auth_id(id);
    const std::string name = player_name(id);
    const std::string safe_name = text_escape(name);
    const std::string safe_auth = text_escape(auth);
    const std::string safe_type = text_escape(ev.type);
    const std::string safe_details = text_escape(ev.details);
    char line[1024];
    std::snprintf(line, sizeof(line),
        "[LiveAC] player=\"%s\" auth=\"%s\" type=%s level=%s score=%.1f time=%.3f details=\"%s\"\n",
        safe_name.c_str(), safe_auth.c_str(), safe_type.c_str(),
        ev.detection ? "DETECTED" : "WARNING", score, ev.time, safe_details.c_str());
    SERVER_PRINT(line);

    const std::string log_path = liveac_log_dir + "/liveac_evidence.log";
    const std::string panel_path = liveac_log_dir + "/panel_events.jsonl";
    rotate_if_needed(log_path);
    rotate_if_needed(panel_path);
    std::ofstream file(log_path, std::ios::app);
    if (file) file << line;
    std::ofstream panel(panel_path, std::ios::app);
    if (panel) {
        panel << "{\"time\":" << ev.time
              << ",\"slot\":" << id
              << ",\"player\":\"" << json_escape(name) << "\""
              << ",\"auth\":\"" << json_escape(auth) << "\""
              << ",\"type\":\"" << json_escape(ev.type) << "\""
              << ",\"details\":\"" << json_escape(ev.details) << "\""
              << ",\"score\":" << score
              << ",\"level\":\"" << (ev.detection ? "DETECTED" : "WARNING") << "\"}\n";
    }
    ++evidence_count[id];
    ++evidence_types[id][ev.type];
    last_evidence_type[id] = ev.type;
    last_evidence_time[id] = ev.time;

    if (watch_active[id]) {
        char watch_line[768];
        std::snprintf(watch_line, sizeof(watch_line),
            "[LiveAC][WATCH] %s: %s (%s), score %.1f - %s\n",
            safe_name.c_str(), safe_type.c_str(), ev.detection ? "DETECTED" : "WARNING",
            score, safe_details.c_str());
        print_to_slot(watch_requester[id], watch_line);
    }
}

void print_player_status(edict_t* receiver, int id) {
    if (!active_player(id) || !detectors[id]) return;
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    char line[640];
    std::snprintf(line, sizeof(line),
        "[LiveAC] #%d name=\"%s\" auth=\"%s\" score=%.1f evidence=%u last=%s at=%.3f%s%s%s\n",
        id, player_name(id).c_str(), auth ? auth : "unknown", detectors[id]->score(),
        evidence_count[id], last_evidence_type[id].empty() ? "none" : last_evidence_type[id].c_str(),
        last_evidence_time[id], is_bot(id) ? " [BOT ignored]" : "",
        scan_active[id] ? " [SCAN]" : "", watch_active[id] ? " [WATCH]" : "");
    print_to(receiver, line);
}

int command_target(edict_t* caller, int argument_index = 1) {
    if (g_engfuncs.pfnCmd_Argc() <= argument_index) return 0;
    const char* arg = g_engfuncs.pfnCmd_Argv(argument_index);
    if (!arg || !*arg) return 0;
    char* end = nullptr;
    const long slot = std::strtol(arg, &end, 10);
    if (end && *end == '\0' && slot >= 1 && slot <= MAX_CLIENTS_LOCAL && active_player(static_cast<int>(slot)))
        return static_cast<int>(slot);

    const std::string needle = lowercase(arg);
    std::vector<int> matches;
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
        if (!active_player(id)) continue;
        if (lowercase(player_name(id)).find(needle) != std::string::npos) matches.push_back(id);
    }
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        std::ostringstream out;
        out << "[LiveAC] Multiple players match \"" << text_escape(arg) << "\":\n";
        for (int id : matches) out << "  #" << id << " " << text_escape(player_name(id)) << "\n";
        out << "Use the numeric slot.\n";
        print_to(caller, out.str());
        return -1;
    }
    return 0;
}

std::string verdict(float score, unsigned int new_evidence, bool enough_data = true) {
    if (!enough_data) return "NOT ENOUGH DATA";
    if (new_evidence == 0 && score < 30.0f) return "CLEAN";
    if (score >= 75.0f) return "HIGHLY SUSPICIOUS";
    if (score >= 55.0f) return "SUSPICIOUS";
    if (score >= 30.0f) return "REVIEW RECOMMENDED";
    return "LOW SUSPICION";
}

void print_scan_report(int id, edict_t* receiver) {
    if (!active_player(id) || !detectors[id]) return;
    const unsigned int new_evidence = evidence_count[id] - scan_start_evidence[id];
    const float current_score = detectors[id]->score();
    const std::string report_auth = auth_id(id);
    const std::string report_name = player_name(id);
    const double elapsed = gpGlobals ? gpGlobals->time - scan_started[id] : 0.0;
    const auto& now = detectors[id]->stats();
    const auto& before = scan_start_stats[id];
    auto delta = [](std::uint64_t a, std::uint64_t b) { return a >= b ? a - b : 0; };

    const auto samples = delta(now.total_samples, before.total_samples);
    const auto visible = delta(now.visible_target_samples, before.visible_target_samples);
    const auto acquisitions = delta(now.target_acquisitions, before.target_acquisitions);
    const auto attacks = delta(now.attack_edges, before.attack_edges);
    const auto attacks_visible = delta(now.attacks_on_visible_target, before.attacks_on_visible_target);
    const auto attacks_crosshair = delta(now.attacks_in_crosshair, before.attacks_in_crosshair);
    const auto reactions = delta(now.reaction_samples, before.reaction_samples);
    const auto jumps = delta(now.jump_edges, before.jump_edges);
    const auto landings = delta(now.valid_landings, before.valid_landings);
    const auto ideal = delta(now.ideal_jumps, before.ideal_jumps);
    const auto speed_samples = delta(now.speed_samples, before.speed_samples);
    const auto speed_anomalies = delta(now.speed_anomalies, before.speed_anomalies);

    auto event_delta = [&](const char* type) -> unsigned int {
        unsigned int current = 0, old = 0;
        auto it = evidence_types[id].find(type); if (it != evidence_types[id].end()) current = it->second;
        auto jt = scan_start_types[id].find(type); if (jt != scan_start_types[id].end()) old = jt->second;
        return current >= old ? current - old : 0;
    };
    const unsigned int aim_events = event_delta("TARGET_AIM_SNAP") + event_delta("UDS_AIM_TYPE_5_CONTEXT") + event_delta("TARGET_REACTION_PATTERN");
    const unsigned int fire_events = event_delta("UDS_AUTOATTACK_CONTEXT");
    const unsigned int movement_events = event_delta("UDS_IDEALJUMP_ADAPTED");

    auto result = [](bool enough, bool warning) { return !enough ? "INSUFFICIENT" : (warning ? "WARNING" : "PASS"); };
    const bool aim_enough = acquisitions >= 8 && visible >= 80;
    const bool fire_enough = attacks >= 20 && attacks_visible >= 5;
    const bool movement_enough = landings >= 8 && jumps >= 8;
    const bool speed_enough = speed_samples >= 300;
    const bool aim_warning = aim_events > 0;
    const bool fire_warning = fire_events > 0;
    const bool movement_warning = movement_events > 0 || (ideal >= 10 && landings >= 10);
    const bool speed_warning = speed_anomalies >= 3;
    const bool enough_overall = samples >= 300 && (aim_enough || fire_enough || movement_enough || speed_enough);

    std::ostringstream out;
    out << "\n========== LiveAC Scan Report ==========\n"
        << "Player: " << text_escape(report_name) << " (#" << id << ")\n"
        << "SteamID: " << text_escape(report_auth) << "\n"
        << "Duration: " << static_cast<int>(elapsed) << " seconds | Samples: " << samples << "\n\n"
        << "AIM [" << result(aim_enough, aim_warning) << "]\n"
        << "  Visible target samples: " << visible << "\n"
        << "  Target acquisitions: " << acquisitions << "\n"
        << "  Reaction samples: " << reactions << "\n"
        << "  Aim detector events: " << aim_events << "\n\n"
        << "FIRE [" << result(fire_enough, fire_warning) << "]\n"
        << "  Attack edges: " << attacks << "\n"
        << "  Attacks on visible target: " << attacks_visible << "\n"
        << "  Attacks in crosshair: " << attacks_crosshair << "\n"
        << "  Fire detector events: " << fire_events << "\n\n"
        << "MOVEMENT [" << result(movement_enough, movement_warning) << "]\n"
        << "  Jump edges: " << jumps << "\n"
        << "  Valid landings: " << landings << "\n"
        << "  Ideal jumps: " << ideal << "\n"
        << "  Movement detector events: " << movement_events << "\n\n"
        << "SPEED [" << result(speed_enough, speed_warning) << "]\n"
        << "  Speed samples: " << speed_samples << "\n"
        << "  Maximum horizontal speed: " << now.max_horizontal_speed << "\n"
        << "  Extreme speed anomalies (>520): " << speed_anomalies << "\n\n"
        << "Score: " << static_cast<int>(current_score) << "/100"
        << " | detector diversity: " << detectors[id]->detector_diversity() << "\n"
        << "New evidence: " << new_evidence << "\n"
        << "Verdict: " << verdict(current_score, new_evidence, enough_overall) << "\n"
        << "Note: INSUFFICIENT means the detector did not receive enough valid situations; it is not PASS.\n"
        << "LiveAC provides evidence for manual review and never auto-bans.\n"
        << "========================================\n";
    print_to(receiver, out.str());

    rotate_if_needed(liveac_log_dir + "/panel_events.jsonl");
    std::ofstream panel(liveac_log_dir + "/panel_events.jsonl", std::ios::app);
    if (panel) {
        panel << "{\"event\":\"scan_report\",\"time\":" << (gpGlobals ? gpGlobals->time : 0.0)
              << ",\"slot\":" << id
              << ",\"player\":\"" << json_escape(report_name) << "\""
              << ",\"auth\":\"" << json_escape(report_auth) << "\""
              << ",\"duration\":" << elapsed << ",\"samples\":" << samples
              << ",\"score\":" << current_score << ",\"new_evidence\":" << new_evidence
              << ",\"aim\":{\"status\":\"" << result(aim_enough, aim_warning) << "\",\"visible_samples\":" << visible << ",\"acquisitions\":" << acquisitions << ",\"reaction_samples\":" << reactions << ",\"events\":" << aim_events << "}"
              << ",\"fire\":{\"status\":\"" << result(fire_enough, fire_warning) << "\",\"attacks\":" << attacks << ",\"visible_attacks\":" << attacks_visible << ",\"crosshair_attacks\":" << attacks_crosshair << ",\"events\":" << fire_events << "}"
              << ",\"movement\":{\"status\":\"" << result(movement_enough, movement_warning) << "\",\"jumps\":" << jumps << ",\"landings\":" << landings << ",\"ideal_jumps\":" << ideal << ",\"events\":" << movement_events << "}"
              << ",\"speed\":{\"status\":\"" << result(speed_enough, speed_warning) << "\",\"samples\":" << speed_samples << ",\"max\":" << now.max_horizontal_speed << ",\"anomalies\":" << speed_anomalies << "}"
              << ",\"verdict\":\"" << verdict(current_score, new_evidence, enough_overall) << "\"}\n";
    }
}
void finish_scan(int id) {
    if (!scan_active[id]) return;
    edict_t* receiver = active_player(scan_requester[id]) ? INDEXENT(scan_requester[id]) : nullptr;
    print_scan_report(id, receiver);
    clear_scan(id);
}

void command_help(edict_t* caller) {
    print_to(caller, "[LiveAC] Admin commands (server/RCON or authorized in-game console):\n");
    print_to(caller, "  liveac_status [slot|name]       - list/inspect players\n");
    print_to(caller, "  liveac_scan <slot|name> [sec]   - focused scan (15-300 sec, default 60)\n");
    print_to(caller, "  liveac_watch <slot|name>        - stream detector events to your console\n");
    print_to(caller, "  liveac_unwatch <slot|name|all>  - stop watch mode\n");
    print_to(caller, "  liveac_top                     - highest current suspicion scores\n");
    print_to(caller, "  liveac_reset <slot|name|all>    - clear score/evidence\n");
    print_to(caller, "  liveac_reload_admins            - reload liveac_admins.ini\n");
    print_to(caller, "  liveac_reload_config            - reload liveac.cfg\n");
    print_to(caller, "  liveac_menu                     - show management menu/help\n");
}

void command_status(edict_t* caller) {
    const int target = command_target(caller);
    if (target > 0) {
        print_player_status(caller, target);
        return;
    }
    print_to(caller, "[LiveAC] Players: slot, score, evidence and last detector\n");
    bool found = false;
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
        if (!active_player(id) || !detectors[id]) continue;
        print_player_status(caller, id);
        found = true;
    }
    if (!found) print_to(caller, "[LiveAC] No active players.\n");
    if (g_engfuncs.pfnCmd_Argc() >= 2 && target == 0)
        print_to(caller, "[LiveAC] Target not found. Use: liveac_status <slot|partial-name>\n");
}

void command_scan(edict_t* caller) {
    const int target = command_target(caller);
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_scan <slot|partial-name> [15-300 seconds]\n");
        return;
    }
    if (is_bot(target) && !config.liveac_allow_bot_scan) {
        print_to(caller, "[LiveAC] Bot scans are disabled. Set liveac_allow_bot_scan=1 only for controlled testing.\n");
        return;
    }
    ensure_player(target);
    int seconds = DEFAULT_SCAN_SECONDS;
    if (g_engfuncs.pfnCmd_Argc() >= 3) seconds = std::atoi(g_engfuncs.pfnCmd_Argv(2));
    seconds = std::max(MIN_SCAN_SECONDS, std::min(MAX_SCAN_SECONDS, seconds));

    scan_active[target] = true;
    scan_started[target] = gpGlobals->time;
    scan_ends[target] = gpGlobals->time + static_cast<double>(seconds);
    scan_start_score[target] = detectors[target]->score();
    scan_start_evidence[target] = evidence_count[target];
    scan_start_types[target] = evidence_types[target];
    scan_start_stats[target] = detectors[target]->stats();
    scan_requester[target] = caller ? player_index(caller) : 0;

    char message[384];
    std::snprintf(message, sizeof(message),
        "[LiveAC] Focused scan started for %s (#%d) for %d seconds. Report will print automatically.\n",
        player_name(target).c_str(), target, seconds);
    print_to(caller, message);
    audit_action(caller, "liveac_scan", player_name(target) + " (#" + std::to_string(target) + ", " + std::to_string(seconds) + "s)");
}

void command_watch(edict_t* caller) {
    const int target = command_target(caller);
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_watch <slot|partial-name>\n");
        return;
    }
    if (is_bot(target) && !config.liveac_allow_bot_scan) {
        print_to(caller, "[LiveAC] Bot scans are disabled. Set liveac_allow_bot_scan=1 only for controlled testing.\n");
        return;
    }
    watch_active[target] = true;
    watch_requester[target] = caller ? player_index(caller) : 0;
    print_to(caller, "[LiveAC] Watch enabled. Detector events will appear in your console.\n");
    audit_action(caller, "liveac_watch", player_name(target) + " (#" + std::to_string(target) + ")");
}

void command_unwatch(edict_t* caller) {
    if (g_engfuncs.pfnCmd_Argc() >= 2 && std::strcmp(g_engfuncs.pfnCmd_Argv(1), "all") == 0) {
        for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
            if (!caller || watch_requester[id] == player_index(caller)) {
                watch_active[id] = false;
                watch_requester[id] = 0;
            }
        }
        print_to(caller, "[LiveAC] Watch disabled for all your targets.\n");
        audit_action(caller, "liveac_unwatch", "all");
        return;
    }
    const int target = command_target(caller);
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_unwatch <slot|partial-name|all>\n");
        return;
    }
    watch_active[target] = false;
    watch_requester[target] = 0;
    print_to(caller, "[LiveAC] Watch disabled.\n");
    audit_action(caller, "liveac_unwatch", player_name(target) + " (#" + std::to_string(target) + ")");
}

void command_top(edict_t* caller) {
    std::vector<std::pair<float, int>> ranking;
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id)
        if (active_player(id) && detectors[id] && !is_bot(id)) ranking.push_back({detectors[id]->score(), id});
    std::sort(ranking.begin(), ranking.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    print_to(caller, "[LiveAC] Top suspicion scores:\n");
    const std::size_t limit = std::min<std::size_t>(10, ranking.size());
    for (std::size_t i = 0; i < limit; ++i) {
        char line[256];
        std::snprintf(line, sizeof(line), "  %u. #%d %-24s %.1f/100 (%u evidence)\n",
            static_cast<unsigned int>(i + 1), ranking[i].second, player_name(ranking[i].second).c_str(),
            ranking[i].first, evidence_count[ranking[i].second]);
        print_to(caller, line);
    }
    if (ranking.empty()) print_to(caller, "  No human players are being analyzed.\n");
}

void command_reset(edict_t* caller) {
    if (g_engfuncs.pfnCmd_Argc() >= 2 && std::strcmp(g_engfuncs.pfnCmd_Argv(1), "all") == 0) {
        for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id)
            if (detectors[id]) reset_player(id);
        print_to(caller, "[LiveAC] All player scores were reset.\n");
        audit_action(caller, "liveac_reset", "all");
        return;
    }
    const int target = command_target(caller);
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_reset <slot|partial-name|all>\n");
        return;
    }
    const std::string reset_name = player_name(target);
    reset_player(target);
    print_to(caller, "[LiveAC] Player score and evidence counters reset.\n");
    audit_action(caller, "liveac_reset", reset_name + " (#" + std::to_string(target) + ")");
}


void command_menu(edict_t* caller) {
    print_to(caller, "\n========== LiveAC Admin Menu ==========\n");
    print_to(caller, "1) liveac_status             - all players\n");
    print_to(caller, "2) liveac_top                - top suspects\n");
    print_to(caller, "3) liveac_scan <slot> 60     - private focused scan\n");
    print_to(caller, "4) liveac_watch <slot>       - private live events\n");
    print_to(caller, "5) liveac_unwatch <slot|all> - stop watch\n");
    print_to(caller, "6) liveac_reload_config      - reload tuning\n");
    print_to(caller, "7) liveac_reload_admins      - reload admins\n");
    print_to(caller, "=======================================\n");
}

void execute_command(edict_t* caller, const char* command) {
    if (!require_admin(caller)) return;
    if (std::strcmp(command, "liveac_help") == 0) command_help(caller);
    else if (std::strcmp(command, "liveac_status") == 0) command_status(caller);
    else if (std::strcmp(command, "liveac_scan") == 0) command_scan(caller);
    else if (std::strcmp(command, "liveac_watch") == 0) command_watch(caller);
    else if (std::strcmp(command, "liveac_unwatch") == 0) command_unwatch(caller);
    else if (std::strcmp(command, "liveac_top") == 0) command_top(caller);
    else if (std::strcmp(command, "liveac_reset") == 0) command_reset(caller);
    else if (std::strcmp(command, "liveac_reload_admins") == 0) {
        load_admins();
        audit_action(caller, command);
        print_to(caller, "[LiveAC] Admin list reloaded.\n");
    }
    else if (std::strcmp(command, "liveac_reload_config") == 0) {
        load_config();
        audit_action(caller, command);
        print_to(caller, "[LiveAC] Configuration reloaded; detector states reset.\n");
    }
    else if (std::strcmp(command, "liveac_menu") == 0) {
        audit_action(caller, command);
        command_menu(caller);
    }
}

void ServerCommandHelp() { execute_command(nullptr, "liveac_help"); }
void ServerCommandStatus() { execute_command(nullptr, "liveac_status"); }
void ServerCommandScan() { execute_command(nullptr, "liveac_scan"); }
void ServerCommandWatch() { execute_command(nullptr, "liveac_watch"); }
void ServerCommandUnwatch() { execute_command(nullptr, "liveac_unwatch"); }
void ServerCommandTop() { execute_command(nullptr, "liveac_top"); }
void ServerCommandReset() { execute_command(nullptr, "liveac_reset"); }
void ServerCommandReloadAdmins() { execute_command(nullptr, "liveac_reload_admins"); }
void ServerCommandReloadConfig() { execute_command(nullptr, "liveac_reload_config"); }
void ServerCommandMenu() { execute_command(nullptr, "liveac_menu"); }


struct TargetContext {
    int slot{};
    bool visible{};
    bool in_crosshair{};
    float angle_error{180.0f};
};

float normalize_angle(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float angular_error_to(const edict_t* viewer, const edict_t* target, const usercmd_s* cmd) {
    const float sx = viewer->v.origin.x + viewer->v.view_ofs.x;
    const float sy = viewer->v.origin.y + viewer->v.view_ofs.y;
    const float sz = viewer->v.origin.z + viewer->v.view_ofs.z;
    const float tx = target->v.origin.x + target->v.view_ofs.x;
    const float ty = target->v.origin.y + target->v.view_ofs.y;
    const float tz = target->v.origin.z + target->v.view_ofs.z;
    const float dx = tx - sx, dy = ty - sy, dz = tz - sz;
    const float flat = std::sqrt(dx * dx + dy * dy);
    constexpr float RAD_TO_DEG = 57.29577951308232f;
    const float target_yaw = std::atan2(dy, dx) * RAD_TO_DEG;
    const float target_pitch = -std::atan2(dz, flat) * RAD_TO_DEG;
    const float yaw_error = normalize_angle(target_yaw - cmd->viewangles.y);
    const float pitch_error = normalize_angle(target_pitch - cmd->viewangles.x);
    return std::sqrt(yaw_error * yaw_error + pitch_error * pitch_error);
}

bool visible_to(const edict_t* viewer, edict_t* target) {
    float start[3] = {
        viewer->v.origin.x + viewer->v.view_ofs.x,
        viewer->v.origin.y + viewer->v.view_ofs.y,
        viewer->v.origin.z + viewer->v.view_ofs.z
    };
    float end[3] = {
        target->v.origin.x + target->v.view_ofs.x,
        target->v.origin.y + target->v.view_ofs.y,
        target->v.origin.z + target->v.view_ofs.z
    };
    TraceResult tr{};
    g_engfuncs.pfnTraceLine(start, end, dont_ignore_monsters, const_cast<edict_t*>(viewer), &tr);
    return tr.pHit == target || tr.flFraction >= 0.999f;
}

TargetContext target_context(const edict_t* viewer, const usercmd_s* cmd) {
    TargetContext result;
    const int viewer_team = viewer->v.team;
    for (int slot = 1; slot <= MAX_CLIENTS_LOCAL; ++slot) {
        if (!active_player(slot) || slot == player_index(viewer)) continue;
        if (is_bot(slot) && !config.liveac_allow_bot_targets) continue;
        edict_t* candidate = INDEXENT(slot);
        if (candidate->v.deadflag != DEAD_NO || candidate->v.health <= 0.0f) continue;
        if (viewer_team > 0 && candidate->v.team == viewer_team) continue;
        const float error = angular_error_to(viewer, candidate, cmd);
        if (error >= result.angle_error || error > 35.0f) continue;
        if (!visible_to(viewer, candidate)) continue;
        result.slot = slot;
        result.visible = true;
        result.angle_error = error;
    }

    // A narrow angular cone is more stable than relying solely on a trace hitting a hitbox.
    result.in_crosshair = result.visible && result.angle_error <= 1.25f;
    return result;
}

void CmdStart(const edict_t* player, const usercmd_s* cmd, unsigned int) {
    if (!player || !cmd || player->free) RETURN_META(MRES_IGNORED);
    const int id = player_index(player);
    if (id < 1 || id > MAX_CLIENTS_LOCAL || !player->pvPrivateData) RETURN_META(MRES_IGNORED);
    if (is_bot(id) && !config.liveac_allow_bot_scan) RETURN_META(MRES_IGNORED);

    ensure_player(id);
    liveac::Sample sample;
    sample.time = gpGlobals->time;
    sample.command_number = ++cmd_sequence[id];
    sample.pitch = cmd->viewangles.x;
    sample.yaw = cmd->viewangles.y;
    sample.forwardmove = cmd->forwardmove;
    sample.sidemove = cmd->sidemove;
    sample.buttons = static_cast<std::uint32_t>(cmd->buttons);
    sample.on_ground = (player->v.flags & FL_ONGROUND) != 0;
    sample.alive = player->v.deadflag == DEAD_NO && player->v.health > 0.0f;
    if (cmd->weaponselect > 0) current_weapon[id] = cmd->weaponselect;
    sample.weapon_id = current_weapon[id];
    sample.horizontal_speed = std::sqrt(player->v.velocity.x * player->v.velocity.x + player->v.velocity.y * player->v.velocity.y);

    const TargetContext context = target_context(player, cmd);
    sample.target_slot = context.slot;
    sample.target_visible = context.visible;
    sample.target_in_crosshair = context.in_crosshair;
    sample.target_angle_error = context.angle_error;
    sample.previous_target_angle_error = previous_target_error[id];
    previous_target_error[id] = context.visible ? context.angle_error : 180.0f;

    const auto evidence = detectors[id]->push(sample);
    for (const auto& item : evidence) log_evidence(id, item, detectors[id]->score());

    if (scan_active[id] && gpGlobals->time >= scan_ends[id]) finish_scan(id);

    const float score = detectors[id]->score();
    if (score >= config.alert_score && gpGlobals->time - last_alert[id] >= 15.0f) {
        char message[320];
        std::snprintf(message, sizeof(message),
            "[LiveAC] %s suspicion %.0f/100 - use liveac_status %d or liveac_scan %d 60\n",
            player_name(id).c_str(), score, id, id);
        SERVER_PRINT(message);
        last_alert[id] = gpGlobals->time;
    }
    RETURN_META(MRES_IGNORED);
}

qboolean ClientConnect(edict_t* ent, const char*, const char*, char[128]) {
    reset_player(player_index(ent));
    RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientDisconnect(edict_t* ent) {
    const int id = player_index(ent);
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL) {
        if (scan_active[id]) finish_scan(id);
        detectors[id].reset();
        cmd_sequence[id] = 0;
    previous_target_error[id] = 180.0f;
    current_weapon[id] = 0;
        evidence_count[id] = 0;
        evidence_types[id].clear();
        last_evidence_type[id].clear();
        last_evidence_time[id] = 0.0;
        clear_scan(id);
        watch_active[id] = false;
        watch_requester[id] = 0;
        for (int target = 1; target <= MAX_CLIENTS_LOCAL; ++target) {
            if (scan_requester[target] == id) scan_requester[target] = 0;
            if (watch_requester[target] == id) {
                watch_active[target] = false;
                watch_requester[target] = 0;
            }
        }
    }
    RETURN_META(MRES_IGNORED);
}

void ClientCommand(edict_t* ent) {
    if (!ent || ent->free || g_engfuncs.pfnCmd_Argc() < 1) RETURN_META(MRES_IGNORED);
    const char* command = g_engfuncs.pfnCmd_Argv(0);
    if (!command || std::strncmp(command, "liveac_", 7) != 0) RETURN_META(MRES_IGNORED);
    execute_command(ent, command);
    RETURN_META(MRES_SUPERCEDE);
}
} // namespace

C_DLLEXPORT int Meta_Query(const char*, plugin_info_t** pluginInfo, mutil_funcs_t* metaUtilFuncs) {
    gpMetaUtilFuncs = metaUtilFuncs;
    *pluginInfo = &Plugin_info;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS* metaFunctionTable,
                            meta_globals_t* metaGlobals, gamedll_funcs_t* gameDllFuncs) {
    gpMetaGlobals = metaGlobals;
    gpGamedllFuncs = gameDllFuncs;
    metaFunctionTable->pfnGetEntityAPI2 = GetEntityAPI2;

    g_engfuncs.pfnAddServerCommand("liveac_help", ServerCommandHelp);
    g_engfuncs.pfnAddServerCommand("liveac_status", ServerCommandStatus);
    g_engfuncs.pfnAddServerCommand("liveac_scan", ServerCommandScan);
    g_engfuncs.pfnAddServerCommand("liveac_watch", ServerCommandWatch);
    g_engfuncs.pfnAddServerCommand("liveac_unwatch", ServerCommandUnwatch);
    g_engfuncs.pfnAddServerCommand("liveac_top", ServerCommandTop);
    g_engfuncs.pfnAddServerCommand("liveac_reset", ServerCommandReset);
    g_engfuncs.pfnAddServerCommand("liveac_reload_admins", ServerCommandReloadAdmins);
    g_engfuncs.pfnAddServerCommand("liveac_reload_config", ServerCommandReloadConfig);
    g_engfuncs.pfnAddServerCommand("liveac_menu", ServerCommandMenu);

    char game_dir[256]{};
    GET_GAME_DIR(game_dir);
    liveac_base_dir = std::string(game_dir) + "/addons/liveac";
    liveac_log_dir = liveac_base_dir + "/logs";
    config_file_path = liveac_base_dir + "/liveac.cfg";
    ensure_directory(liveac_base_dir);
    ensure_directory(liveac_log_dir);
    load_config();
    load_admins();
    SERVER_PRINT("[LiveAC] v1.0 loaded. Developed by IDO, MIT licensed; bots ignored; no automatic ban.\n");
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) { return TRUE; }

C_DLLEXPORT void GiveFnptrsToDll(enginefuncs_t* engineFuncs, globalvars_t* globals) {
    std::memcpy(&g_engfuncs, engineFuncs, sizeof(enginefuncs_t));
    gpGlobals = globals;
}

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS* table, int* interfaceVersion) {
    if (!table || !interfaceVersion) return FALSE;
    if (*interfaceVersion != INTERFACE_VERSION) {
        *interfaceVersion = INTERFACE_VERSION;
        return FALSE;
    }
    std::memset(table, 0, sizeof(DLL_FUNCTIONS));
    table->pfnCmdStart = CmdStart;
    table->pfnClientConnect = ClientConnect;
    table->pfnClientDisconnect = ClientDisconnect;
    table->pfnClientCommand = ClientCommand;
    return TRUE;
}
