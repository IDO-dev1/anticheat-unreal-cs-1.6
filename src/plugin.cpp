// Standard C++ headers must be loaded before legacy HLSDK headers.
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

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
    "0.4.0",
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
std::array<std::unique_ptr<liveac::PlayerDetector>, MAX_CLIENTS_LOCAL + 1> detectors;
std::array<float, MAX_CLIENTS_LOCAL + 1> last_alert{};
std::array<std::uint64_t, MAX_CLIENTS_LOCAL + 1> cmd_sequence{};
std::array<unsigned int, MAX_CLIENTS_LOCAL + 1> evidence_count{};
std::array<std::string, MAX_CLIENTS_LOCAL + 1> last_evidence_type{};
std::array<double, MAX_CLIENTS_LOCAL + 1> last_evidence_time{};
liveac::Config config;

int player_index(const edict_t* ent) { return ENTINDEX(const_cast<edict_t*>(ent)); }

bool is_bot(int id) {
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    return auth && std::strcmp(auth, "BOT") == 0;
}

void ensure_player(int id) {
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL && !detectors[id])
        detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
}

void reset_player(int id) {
    if (id < 1 || id > MAX_CLIENTS_LOCAL) return;
    detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
    last_alert[id] = 0.0f;
    cmd_sequence[id] = 0;
    evidence_count[id] = 0;
    last_evidence_type[id].clear();
    last_evidence_time[id] = 0.0;
}

void print_server(const std::string& text) {
    SERVER_PRINT(text.c_str());
}

void log_evidence(int id, const liveac::Evidence& ev, float score) {
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    const char* name = STRING(INDEXENT(id)->v.netname);
    char line[1024];
    std::snprintf(line, sizeof(line),
        "[LiveAC] player=\"%s\" auth=\"%s\" type=%s level=%s score=%.1f time=%.3f details=\"%s\"\n",
        name ? name : "unknown", auth ? auth : "unknown", ev.type.c_str(),
        ev.detection ? "DETECTED" : "WARNING", score, ev.time, ev.details.c_str());
    SERVER_PRINT(line);
    std::ofstream f("liveac_evidence.log", std::ios::app);
    if (f) f << line;
    ++evidence_count[id];
    last_evidence_type[id] = ev.type;
    last_evidence_time[id] = ev.time;
}

void print_player_status(int id) {
    if (id < 1 || id > MAX_CLIENTS_LOCAL) return;
    edict_t* ent = INDEXENT(id);
    if (!ent || ent->free || !ent->pvPrivateData || !detectors[id]) return;
    const char* name = STRING(ent->v.netname);
    const char* auth = GETPLAYERAUTHID(ent);
    char line[512];
    std::snprintf(line, sizeof(line),
        "[LiveAC] #%d name=\"%s\" auth=\"%s\" score=%.1f evidence=%u last=%s at=%.3f%s\n",
        id, name ? name : "unknown", auth ? auth : "unknown", detectors[id]->score(),
        evidence_count[id], last_evidence_type[id].empty() ? "none" : last_evidence_type[id].c_str(),
        last_evidence_time[id], is_bot(id) ? " [BOT ignored]" : "");
    SERVER_PRINT(line);
}

int command_target() {
    if (g_engfuncs.pfnCmd_Argc() < 2) return 0;
    const char* arg = g_engfuncs.pfnCmd_Argv(1);
    if (!arg || !*arg) return 0;
    char* end = nullptr;
    const long slot = std::strtol(arg, &end, 10);
    if (end && *end == '\0' && slot >= 1 && slot <= MAX_CLIENTS_LOCAL)
        return static_cast<int>(slot);

    std::string needle(arg);
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
        edict_t* ent = INDEXENT(id);
        if (!ent || ent->free || !ent->pvPrivateData) continue;
        std::string name = STRING(ent->v.netname);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.find(needle) != std::string::npos) return id;
    }
    return 0;
}

void CommandStatus() {
    const int target = command_target();
    if (target > 0) {
        print_player_status(target);
        return;
    }
    print_server("[LiveAC] Players: slot, score, evidence and last detector\n");
    bool found = false;
    for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id) {
        edict_t* ent = INDEXENT(id);
        if (!ent || ent->free || !ent->pvPrivateData || !detectors[id]) continue;
        print_player_status(id);
        found = true;
    }
    if (!found) print_server("[LiveAC] No active players.\n");
    if (g_engfuncs.pfnCmd_Argc() >= 2 && target == 0)
        print_server("[LiveAC] Target not found. Use: liveac_status <slot|partial-name>\n");
}

void CommandReset() {
    if (g_engfuncs.pfnCmd_Argc() >= 2) {
        const char* arg = g_engfuncs.pfnCmd_Argv(1);
        if (arg && std::strcmp(arg, "all") == 0) {
            for (int id = 1; id <= MAX_CLIENTS_LOCAL; ++id)
                if (detectors[id]) reset_player(id);
            print_server("[LiveAC] All player scores were reset.\n");
            return;
        }
    }
    const int target = command_target();
    if (target <= 0) {
        print_server("[LiveAC] Usage: liveac_reset <slot|partial-name|all>\n");
        return;
    }
    reset_player(target);
    print_server("[LiveAC] Player score and evidence counters reset.\n");
}

void CommandHelp() {
    print_server("[LiveAC] Commands (server console/RCON):\n");
    print_server("  liveac_status                    - list active players\n");
    print_server("  liveac_status <slot|name>        - inspect one player\n");
    print_server("  liveac_reset <slot|name|all>     - clear score/evidence counters\n");
    print_server("  liveac_help                      - show this help\n");
}

void CmdStart(const edict_t* player, const usercmd_s* cmd, unsigned int) {
    if (!player || !cmd || player->free) RETURN_META(MRES_IGNORED);
    const int id = player_index(player);
    if (id < 1 || id > MAX_CLIENTS_LOCAL || !player->pvPrivateData) RETURN_META(MRES_IGNORED);

    // Fake clients produce deterministic, non-human angle changes and are calibration noise.
    if (is_bot(id)) RETURN_META(MRES_IGNORED);

    ensure_player(id);
    liveac::Sample s;
    s.time = gpGlobals->time;
    s.command_number = ++cmd_sequence[id];
    s.pitch = cmd->viewangles.x;
    s.yaw = cmd->viewangles.y;
    s.forwardmove = cmd->forwardmove;
    s.sidemove = cmd->sidemove;
    s.buttons = static_cast<std::uint32_t>(cmd->buttons);
    s.on_ground = (player->v.flags & FL_ONGROUND) != 0;
    s.alive = player->v.deadflag == DEAD_NO && player->v.health > 0.0f;

    const auto evidence = detectors[id]->push(s);
    for (const auto& ev : evidence) log_evidence(id, ev, detectors[id]->score());

    const float score = detectors[id]->score();
    if (score >= config.alert_score && gpGlobals->time - last_alert[id] >= 15.0f) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[LiveAC] %s suspicion %.0f/100 - use liveac_status %d\n",
            STRING(player->v.netname), score, id);
        SERVER_PRINT(msg);
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
        detectors[id].reset();
        cmd_sequence[id] = 0;
        evidence_count[id] = 0;
        last_evidence_type[id].clear();
        last_evidence_time[id] = 0.0;
    }
    RETURN_META(MRES_IGNORED);
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
    g_engfuncs.pfnAddServerCommand("liveac_status", CommandStatus);
    g_engfuncs.pfnAddServerCommand("liveac_reset", CommandReset);
    g_engfuncs.pfnAddServerCommand("liveac_help", CommandHelp);
    SERVER_PRINT("[LiveAC] v0.4 loaded. Type liveac_help in server console.\n");
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
    return TRUE;
}
