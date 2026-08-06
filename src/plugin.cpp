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
    "0.6.0",
    "2026-08-06",
    "Live adaptation with UnrealDemoScanner-derived detectors",
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
std::array<int, MAX_CLIENTS_LOCAL + 1> scan_requester{};

std::array<bool, MAX_CLIENTS_LOCAL + 1> watch_active{};
std::array<int, MAX_CLIENTS_LOCAL + 1> watch_requester{};

std::set<std::string> admin_steamids;
std::string admins_file_path;
liveac::Config config;

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

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
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
    scan_requester[id] = 0;
}

void reset_player(int id) {
    if (id < 1 || id > MAX_CLIENTS_LOCAL) return;
    detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
    last_alert[id] = 0.0f;
    cmd_sequence[id] = 0;
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
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    const std::string name = player_name(id);
    char line[1024];
    std::snprintf(line, sizeof(line),
        "[LiveAC] player=\"%s\" auth=\"%s\" type=%s level=%s score=%.1f time=%.3f details=\"%s\"\n",
        name.c_str(), auth ? auth : "unknown", ev.type.c_str(),
        ev.detection ? "DETECTED" : "WARNING", score, ev.time, ev.details.c_str());
    SERVER_PRINT(line);
    char game_dir[256]{};
    GET_GAME_DIR(game_dir);
    const std::string log_dir = std::string(game_dir) + "/addons/liveac/logs";
    const std::string log_path = log_dir + "/liveac_evidence.log";
    const std::string panel_path = log_dir + "/panel_events.jsonl";
    std::string mkdir_cmd = "mkdir -p \"" + log_dir + "\"";
    std::system(mkdir_cmd.c_str());
    std::ofstream file(log_path, std::ios::app);
    if (file) file << line;
    std::ofstream panel(panel_path, std::ios::app);
    if (panel) {
        panel << "{\"time\":" << ev.time
              << ",\"slot\":" << id
              << ",\"player\":\"" << name << "\""
              << ",\"auth\":\"" << (auth ? auth : "unknown") << "\""
              << ",\"type\":\"" << ev.type << "\""
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
            name.c_str(), ev.type.c_str(), ev.detection ? "DETECTED" : "WARNING",
            score, ev.details.c_str());
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

int command_target(int argument_index = 1) {
    if (g_engfuncs.pfnCmd_Argc() <= argument_index) return 0;
    const char* arg = g_engfuncs.pfnCmd_Argv(argument_index);
    if (!arg || !*arg) return 0;
    char* end = nullptr;
    const long slot = std::strtol(arg, &end, 10);
    if (end && *end == '\0' && slot >= 1 && slot <= MAX_CLIENTS_LOCAL && active_player(static_cast<int>(slot)))
        return static_cast<int>(slot);

    std::string needle(arg);
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
        if (!active_player(id)) continue;
        std::string name = player_name(id);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.find(needle) != std::string::npos) return id;
    }
    return 0;
}

std::string verdict(float score, unsigned int new_evidence) {
    if (new_evidence == 0 && score < 30.0f) return "CLEAN / NO EVIDENCE";
    if (score >= 75.0f) return "HIGHLY SUSPICIOUS";
    if (score >= 55.0f) return "SUSPICIOUS";
    if (score >= 30.0f) return "REVIEW RECOMMENDED";
    return "LOW SUSPICION";
}

void print_scan_report(int id, edict_t* receiver) {
    if (!active_player(id) || !detectors[id]) return;
    const unsigned int new_evidence = evidence_count[id] - scan_start_evidence[id];
    const float current_score = detectors[id]->score();
    const double elapsed = gpGlobals ? gpGlobals->time - scan_started[id] : 0.0;

    std::ostringstream out;
    out << "\n========== LiveAC Scan Report ==========\n"
        << "Player: " << player_name(id) << " (#" << id << ")\n"
        << "SteamID: " << (GETPLAYERAUTHID(INDEXENT(id)) ? GETPLAYERAUTHID(INDEXENT(id)) : "unknown") << "\n"
        << "Duration: " << static_cast<int>(elapsed) << " seconds\n"
        << "Score: " << static_cast<int>(current_score) << "/100"
        << " | detector diversity: " << detectors[id]->detector_diversity()
        << " (started at " << static_cast<int>(scan_start_score[id]) << ")\n"
        << "New evidence: " << new_evidence << "\n";

    bool any_type = false;
    for (const auto& item : evidence_types[id]) {
        unsigned int before = 0;
        const auto previous = scan_start_types[id].find(item.first);
        if (previous != scan_start_types[id].end()) before = previous->second;
        if (item.second > before) {
            out << "  " << item.first << ": " << (item.second - before) << "\n";
            any_type = true;
        }
    }
    if (!any_type) out << "  No detector events during this scan.\n";
    out << "Verdict: " << verdict(current_score, new_evidence) << "\n"
        << "Note: LiveAC provides evidence for manual review; it does not auto-ban.\n"
        << "========================================\n";
    print_to(receiver, out.str());
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
}

void command_status(edict_t* caller) {
    const int target = command_target();
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
    const int target = command_target();
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_scan <slot|partial-name> [15-300 seconds]\n");
        return;
    }
    if (is_bot(target)) {
        print_to(caller, "[LiveAC] Bots are ignored because their deterministic aim creates false positives.\n");
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
    scan_requester[target] = caller ? player_index(caller) : 0;

    char message[384];
    std::snprintf(message, sizeof(message),
        "[LiveAC] Focused scan started for %s (#%d) for %d seconds. Report will print automatically.\n",
        player_name(target).c_str(), target, seconds);
    print_to(caller, message);
}

void command_watch(edict_t* caller) {
    const int target = command_target();
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_watch <slot|partial-name>\n");
        return;
    }
    if (is_bot(target)) {
        print_to(caller, "[LiveAC] Bots are ignored.\n");
        return;
    }
    watch_active[target] = true;
    watch_requester[target] = caller ? player_index(caller) : 0;
    print_to(caller, "[LiveAC] Watch enabled. Detector events will appear in your console.\n");
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
        return;
    }
    const int target = command_target();
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_unwatch <slot|partial-name|all>\n");
        return;
    }
    watch_active[target] = false;
    watch_requester[target] = 0;
    print_to(caller, "[LiveAC] Watch disabled.\n");
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
        return;
    }
    const int target = command_target();
    if (target <= 0) {
        print_to(caller, "[LiveAC] Usage: liveac_reset <slot|partial-name|all>\n");
        return;
    }
    reset_player(target);
    print_to(caller, "[LiveAC] Player score and evidence counters reset.\n");
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
        print_to(caller, "[LiveAC] Admin list reloaded.\n");
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

void CmdStart(const edict_t* player, const usercmd_s* cmd, unsigned int) {
    if (!player || !cmd || player->free) RETURN_META(MRES_IGNORED);
    const int id = player_index(player);
    if (id < 1 || id > MAX_CLIENTS_LOCAL || !player->pvPrivateData) RETURN_META(MRES_IGNORED);
    if (is_bot(id)) RETURN_META(MRES_IGNORED);

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

    load_admins();
    SERVER_PRINT("[LiveAC] v0.6 loaded. Human players are analyzed automatically; manual scan reports are private to requester.\n");
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
