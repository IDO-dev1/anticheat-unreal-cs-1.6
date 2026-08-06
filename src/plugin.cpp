// Load the modern C++ standard library before the legacy HLSDK headers.
// extdll.h defines min/max macros before including math.h; with modern GCC
// those macros otherwise corrupt std::numeric_limits and <cmath>.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "detector.hpp"

#include <extdll.h>
#include <usercmd.h>  // defines usercmd_s/usercmd_t fields used by CmdStart
#include <meta_api.h>
#include <dllapi.h>
#include <engine_api.h>

// Do not let the legacy HLSDK min/max macros leak into later C++ code.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif


plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "Live Unreal Scanner",
    "0.3.0",
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
liveac::Config config;

int player_index(const edict_t* ent) { return ENTINDEX(const_cast<edict_t*>(ent)); }

void ensure_player(int id) {
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL && !detectors[id])
        detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
}

void log_evidence(int id, const liveac::Evidence& ev, float score) {
    const char* auth = GETPLAYERAUTHID(INDEXENT(id));
    const char* name = STRING(INDEXENT(id)->v.netname);
    char line[1024];
    std::snprintf(line, sizeof(line),
        "[LiveAC] player=\"%s\" auth=\"%s\" type=%s level=%s score=%.1f time=%.3f details=\"%s\"\n",
        name ? name : "unknown", auth ? auth : "unknown", ev.type.c_str(), ev.detection ? "DETECTED" : "WARNING", score, ev.time, ev.details.c_str());
    SERVER_PRINT(line);
    std::ofstream f("liveac_evidence.log", std::ios::app);
    if (f) f << line;
}

void CmdStart(const edict_t* player, const usercmd_s* cmd, unsigned int) {
    if (!player || !cmd || player->free) RETURN_META(MRES_IGNORED);
    const int id = player_index(player);
    if (id < 1 || id > MAX_CLIENTS_LOCAL || !player->pvPrivateData) RETURN_META(MRES_IGNORED);
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
        std::snprintf(msg, sizeof(msg), "[LiveAC] %s suspicion %.0f/100 - review evidence log\n",
            STRING(player->v.netname), score);
        SERVER_PRINT(msg);
        last_alert[id] = gpGlobals->time;
    }
    RETURN_META(MRES_IGNORED);
}

qboolean ClientConnect(edict_t* ent, const char*, const char*, char[128]) {
    const int id = player_index(ent);
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL) {
        detectors[id] = std::make_unique<liveac::PlayerDetector>(config);
        last_alert[id] = 0.0f;
        cmd_sequence[id] = 0;
    }
    RETURN_META_VALUE(MRES_IGNORED, TRUE);
}

void ClientDisconnect(edict_t* ent) {
    const int id = player_index(ent);
    if (id >= 1 && id <= MAX_CLIENTS_LOCAL) { detectors[id].reset(); cmd_sequence[id] = 0; }
    RETURN_META(MRES_IGNORED);
}
}

C_DLLEXPORT int Meta_Query(const char* interfaceVersion, plugin_info_t** pluginInfo, mutil_funcs_t* metaUtilFuncs) {
    gpMetaUtilFuncs = metaUtilFuncs;
    *pluginInfo = &Plugin_info;
    return TRUE;
}

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS* metaFunctionTable,
                            meta_globals_t* metaGlobals, gamedll_funcs_t* gameDllFuncs) {
    gpMetaGlobals = metaGlobals;
    gpGamedllFuncs = gameDllFuncs;
    metaFunctionTable->pfnGetEntityAPI2 = GetEntityAPI2;
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
