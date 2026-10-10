/*
 * nevr_plugin_interface.h - the nEVR plugin ABI, version 5.
 *
 * Plugins export plain C functions that the host looks up by name and calls at
 * fixed points in the game's life. Two hosts implement this ABI:
 *
 *   - EchoLoader 2 (BugSplat64.dll, github.com/marshmallow-mia/EchoVR_Mod_Loader)
 *   - nevr-runtime (github.com/EchoTools/nevr-runtime)
 *
 * Written for this project against the published ABI (export names, struct
 * layouts, version rules); see EchoLoader's docs/formats.md. Layouts are x64.
 */

#pragma once

#include <cstdint>

/* Returned by NvrPluginGetInfo, by value (32 bytes). The strings must have
 * static storage: the host keeps the pointers. */
struct NvrPluginInfo {
    const char* name;           /* short identifier, e.g. "asset_patches" */
    const char* description;    /* one line for logs and the launcher */
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
};

/* What get_plugin_info() returns for each loaded plugin (v5). */
struct NvrLoadedPluginInfo {
    const char* name;
    const char* description;
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
    uint32_t    api_version;    /* what the plugin reported (1 when it didn't) */
    uint32_t    capabilities;   /* NvrPluginCapabilities bits (0 = undeclared) */
};

/* Handed to Init/InitEx/OnFrame/OnGameStateChange. The host owns it; it is valid
 * only during the call. v1-v4 hosts pass the first 24 bytes only, so read the
 * v5 fields only when ctx_size says they are there. */
struct NvrGameContext {
    uintptr_t   base_addr;      /* echovr.exe image base (0: not supplied) */
    void*       net_game;       /* CR15NetGame* when NEVR_HOST_HAS_NETGAME */
    uint32_t    game_state;     /* the net game state, for state changes */
    uint32_t    flags;          /* NvrHostFlags */
    /* v5 */
    uint32_t    ctx_size;       /* sizeof(NvrGameContext) as the host built it */
    int         (*get_plugin_count)(void);
    const NvrLoadedPluginInfo* (*get_plugin_info)(int index);
};

enum NvrHostFlags : uint32_t {
    NEVR_HOST_HAS_NETGAME = 0x01,   /* net_game is valid */
    NEVR_HOST_IS_SERVER   = 0x02,
    NEVR_HOST_IS_CLIENT   = 0x04,
    NEVR_HOST_COMBAT_MODE = 0x08,
    NEVR_HOST_IS_HEADLESS = 0x10,
};

/* What a plugin declares it does. Hosts order Init by these bands (lowest
 * first) and show them to players; nothing enforces them. */
enum NvrPluginCapabilities : uint32_t {
    NEVR_PLUGIN_CAP_UNDECLARED      = 0x00,
    NEVR_PLUGIN_CAP_OBSERVES_ONLY   = 0x01,
    NEVR_PLUGIN_CAP_COSMETIC        = 0x02,
    NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY = 0x04,
    NEVR_PLUGIN_CAP_ALTERS_RULES    = 0x08,
    NEVR_PLUGIN_CAP_NETWORK         = 0x10,
    NEVR_PLUGIN_CAP_HOOKS_ENGINE    = 0x20,  /* installs its own detours */
};

/*
 * Exports, in the order a host uses them. Only NvrPluginGetInfo is required.
 *
 *   NvrPluginGetInfo()                  metadata
 *   NvrPluginGetApiVersion()            ABI version built against (absent = 1)
 *   NvrPluginGetCapabilities()          NvrPluginCapabilities bits (absent = 0)
 *   NvrPluginInitEx(ctx, args_json)     once; preferred over Init. args_json is a
 *                                       flat JSON object of strings ("{}" when
 *                                       there are none; nested keys as dotted
 *                                       paths). 0 = ok, nonzero = drop the plugin.
 *   NvrPluginInit(ctx)                  once, when there is no InitEx
 *   NvrPluginOnFrame(ctx)               each game frame
 *   NvrPluginOnGameStateChange(ctx, old, new)
 *   NvrPluginShutdown()                 before a clean unload
 *
 * The version only goes up for incompatible changes; v4 (InitEx) and v5 (the
 * context's tail) only added things, so a newer plugin still runs on an older
 * host and the other way round.
 */
#define NEVR_PLUGIN_API_VERSION 5

typedef NvrPluginInfo (*NvrPluginGetInfo_fn)(void);
typedef uint32_t      (*NvrPluginGetApiVersion_fn)(void);
typedef uint32_t      (*NvrPluginGetCapabilities_fn)(void);
typedef int           (*NvrPluginInitEx_fn)(const NvrGameContext* ctx, const char* args_json);
typedef int           (*NvrPluginInit_fn)(const NvrGameContext* ctx);
typedef void          (*NvrPluginOnFrame_fn)(const NvrGameContext* ctx);
typedef void          (*NvrPluginOnGameStateChange_fn)(const NvrGameContext* ctx,
                                                       uint32_t old_state,
                                                       uint32_t new_state);
typedef void          (*NvrPluginShutdown_fn)(void);

static_assert(sizeof(void*) != 8 || sizeof(NvrPluginInfo) == 32, "NvrPluginInfo layout");
static_assert(sizeof(void*) != 8 || sizeof(NvrLoadedPluginInfo) == 40, "NvrLoadedPluginInfo layout");
static_assert(sizeof(void*) != 8 || sizeof(NvrGameContext) == 48, "NvrGameContext layout");
