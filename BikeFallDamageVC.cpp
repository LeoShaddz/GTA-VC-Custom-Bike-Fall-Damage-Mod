// BikeFallDamageVC.cpp - GTA Vice City 1.0 (PC, gta-vc.exe)   [NOT COMPILED, NOT TESTED]
// Lets you choose in the .ini how much health AND armor the player loses when he falls off a motorbike. Default: 2 points
// (the game's maximum health is 150). An option makes bike falls never touch the armor (only the health is damaged).
//
// How it works (what was found by reading gta-vc.exe):
//   - CBike::KnockOffRider (0x613920) is what throws a rider off a bike. It does NOT remove health itself.
//   - The health is lost when the ped hits the ground hard: in the ped collision code the game calls
//     CPed::InflictDamage(NULL, WEAPONTYPE_FALL (0x2B), 15.0, ...) from the call at 0x509776 (fixed 15 points).
//   - So: we note the moment the PLAYER is knocked off a bike (hook on every call to KnockOffRider), and when the game then
//     applies that FALL damage to the player within a few seconds, we replace the 15 points with FallDamage from the .ini.
//     (InflictDamage never takes armor for WEAPONTYPE_FALL, so the armor part is done by the mod itself, see ApplyFall.)
//   - The ARMOR is lost in the crash itself: CBike::VehicleDamage calls CPed::InflictDamage(.., WEAPONTYPE_RUNOVERBYCAR (0x27),
//     <damage computed from the impact>, ..) on the rider (calls at 0x614C49 and 0x614CC0) right before knocking him off
//     (it only knocks off a ped whose state is 0x32). For the player, in that case, we replace the computed damage with FallDamage:
//     the game applies it armor first, the remainder goes to the health (all of it to the health when there is no armor).
//     With NoArmorDamage=1 the armor is protected and all of it goes to the health.
//     When the crash damage was replaced, the landing damage that follows in the same fall is skipped (no double damage).
//   - While a bike falls over / lies on its side the game also hits the rider with WEAPONTYPE_FALL damage of about 1 point
//     (CTimer::ms_fTimeStep) on every frame (calls at 0x6137BD and 0x613887, health only). That is replaced too.
//   - One fall = one damage: the first of these damage calls that reaches the player applies FallDamage (armor first, then
//     health); the others during BikeFallWindowMs are skipped, so the total is FallDamage.
// Configuration in BikeFallDamageVC.ini (reloaded when saved).
// Compile as Win32 (x86). The output must be named BikeFallDamageVC.asi
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// ======================= Addresses (GTA VC 1.0) =======================
static const uintptr_t VERSION_ADDR = 0x667BF0;          // start of the 1.0 exe entry point
static const uint32_t  VERSION_OK   = 0x53E58955;
static const uintptr_t FN_KNOCK_OFF_RIDER = 0x613920;    // CBike::KnockOffRider(weapon, direction, CPed* rider, bool always) (thiscall, ret 0x10)
static const uintptr_t FN_INFLICT_DAMAGE  = 0x525B20;    // CPed::InflictDamage(damager, weapon, damage, piece, direction) (thiscall, ret 0x14)
static const uintptr_t SITE_FALL_DAMAGE   = 0x509776;    // the call that applies the 15 points of FALL damage on a hard landing
static const uintptr_t SITES_KNOCK_OFF[] = {             // every call to KnockOffRider in the exe
    0x5009E9, 0x5169C1, 0x516BEE, 0x522C17, 0x522C2F, 0x522D33, 0x522D4B, 0x526960,
    0x5D31E2, 0x60A197, 0x60A1AF, 0x6137D3, 0x6138A0, 0x614C6B, 0x614CE0 };
static const uintptr_t SITES_TIP_DAMAGE[] = { 0x6137BD, 0x613887 };    // bike falling over: FALL damage of about 1 point per frame (driver / passengers)
static const uintptr_t SITES_CRASH_DAMAGE[] = { 0x614C49, 0x614CC0 };  // CBike::VehicleDamage: InflictDamage on the driver / on the passenger
static const uintptr_t ADDR_PLAYER_DAMAGE_MULT = 0x694254;  // float 0.33: CPed::InflictDamage multiplies the damage taken by the PLAYER by this
static const uintptr_t ADDR_PLAYERS      = 0x94AD28;     // CWorld::Players[] (stride 0x170, the ped pointer is at offset 0)
static const uintptr_t ADDR_PLAYER_FOCUS = 0xA10AFB;     // CWorld::PlayerInFocus (byte)
static const uintptr_t ADDR_TIMER_MS     = 0x974B2C;     // CTimer::m_snTimeInMilliseconds (stops when the game is paused)
static const uintptr_t OFF_PED_HEALTH    = 0x354;        // CPed::m_fHealth (float)
static const uintptr_t OFF_PED_ARMOUR    = 0x358;        // CPed::m_fArmour (float)
static const uintptr_t OFF_PED_STATE     = 0x244;        // CPed::m_nPedState (int)
static const int       PED_STATE_DRIVING = 0x32;         // the state VehicleDamage checks before knocking the rider off
static const int       WEAPON_CRASH      = 0x27;         // weapon type VehicleDamage uses for the crash damage
static const int       WEAPON_FALL       = 0x2B;         // WEAPONTYPE_FALL

// ======================= Configuration =======================
static char  g_iniPath[MAX_PATH] = "BikeFallDamageVC.ini";
static bool  g_enabled = true;
static bool  g_logEnabled = false;
static float g_fallDamage = 2.0f;          // health lost on the landing after a bike fall (0 = none). Max health is 150.
static bool  g_noArmorDamage = false;      // 1 = a bike fall never removes armor (only health)
static bool  g_onlyAfterBikeFall = true;   // 1 = only landings soon after the player was knocked off a bike
static DWORD g_windowMs = 5000;            // how long after the knock-off a landing still counts as "the bike fall"
static FILETIME g_lastWrite = {};
static DWORD g_lastCheck = 0;

static void Log(const char* fmt, ...)
{
    if (!g_logEnabled) return;
    FILE* f = fopen("BikeFallDamageVC.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static float ReadFloat(const char* key, float def)
{
    char buf[64];
    GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf), g_iniPath);
    if (!buf[0]) return def;
    for (char* p = buf; *p; ++p) if (*p == ',') *p = '.';
    return (float)atof(buf);
}

static void LoadConfig()
{
    g_enabled = GetPrivateProfileIntA("Settings", "Enabled", 1, g_iniPath) != 0;
    g_logEnabled = GetPrivateProfileIntA("Settings", "EnableLog", 0, g_iniPath) != 0;
    g_fallDamage = ReadFloat("FallDamage", 2.0f);
    if (g_fallDamage < 0.0f) g_fallDamage = 0.0f;
    if (g_fallDamage > 150.0f) g_fallDamage = 150.0f;                   // the game's maximum health
    g_noArmorDamage = GetPrivateProfileIntA("Settings", "NoArmorDamage", 0, g_iniPath) != 0;
    g_onlyAfterBikeFall = GetPrivateProfileIntA("Settings", "OnlyAfterBikeFall", 1, g_iniPath) != 0;
    int w = GetPrivateProfileIntA("Settings", "BikeFallWindowMs", 5000, g_iniPath);
    if (w < 500) w = 500;
    if (w > 60000) w = 60000;
    g_windowMs = (DWORD)w;
}

static void CheckReload()
{
    const DWORD now = GetTickCount();
    if (now - g_lastCheck < 500) return;
    g_lastCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_lastWrite) != 0) { g_lastWrite = fad.ftLastWriteTime; LoadConfig(); Log("Configuration reloaded (FallDamage=%.2f, NoArmorDamage=%d, OnlyAfterBikeFall=%d, window=%lu ms).", g_fallDamage, g_noArmorDamage ? 1 : 0, g_onlyAfterBikeFall ? 1 : 0, (unsigned long)g_windowMs); }
}

// ======================= Game helpers =======================
static void* PlayerPed()
{
    const uint8_t idx = *(const uint8_t*)ADDR_PLAYER_FOCUS;
    return *(void* const*)(ADDR_PLAYERS + (uintptr_t)idx * 0x170);
}
static uint32_t GameTimeMs() { return *(const uint32_t*)ADDR_TIMER_MS; }
static float PedHealth(void* ped) { return ped ? *(const float*)((const uint8_t*)ped + OFF_PED_HEALTH) : -1.0f; }
static float* PedArmour(void* ped) { return (float*)((uint8_t*)ped + OFF_PED_ARMOUR); }

// ======================= Hooks =======================
static bool     g_knocked = false;         // the player was knocked off a bike at g_knockTime
static uint32_t g_knockTime = 0;           // game time (ms) of the knock-off

typedef uint32_t (__thiscall *KnockOff_t)(void* bike, int weapon, int direction, void* rider, int always);
typedef uint8_t  (__thiscall *Inflict_t)(void* ped, void* damager, int weapon, float damage, int piece, int direction);
static Inflict_t g_origInflict = (Inflict_t)FN_INFLICT_DAMAGE;       // target the landing call had before we hooked it

// Replaces every call to CBike::KnockOffRider. Only notes when the player is the rider, then runs the original.
// __fastcall = this in ecx (edx unused) + the other arguments on the stack, callee cleans: same layout as the game's thiscall.
static uint32_t __fastcall Hook_KnockOff(void* bike, void* /*edx*/, int weapon, int direction, void* rider, int always)
{
    CheckReload();
    if (g_enabled && rider && rider == PlayerPed())
    {
        g_knocked = true;
        g_knockTime = GameTimeMs();
        Log("Player knocked off a bike (weapon=%d, health=%.1f, time=%u).", weapon, PedHealth(rider), (unsigned)g_knockTime);
    }
    return ((KnockOff_t)FN_KNOCK_OFF_RIDER)(bike, weapon, direction, rider, always);
}

// ---- One fall = one damage -------------------------------------------------------------------------------------------
// A bike fall can reach several damage calls (the tipping-over damage that the game repeats every frame, the crash damage, the
// hard-landing damage). Only the first one that reaches the player applies FallDamage; the others within BikeFallWindowMs are
// skipped, so the total is always FallDamage.
static bool     g_applied = false;
static uint32_t g_appliedTime = 0;
static bool AppliedRecently() { return g_applied && (uint32_t)(GameTimeMs() - g_appliedTime) <= g_windowMs; }

// Applies FallDamage to the player: the ARMOR absorbs it first and what is left goes to the HEALTH (with no armor all of it goes
// to the health). With NoArmorDamage=1 the armor is not touched and all of it goes to the health.
// The armor part is done here (the game never lets a WEAPONTYPE_FALL hit take armor); the health part is a normal game call.
static uint8_t ApplyFall(void* ped, void* damager, int weapon, int piece, int direction, const char* source, float gameDamage)
{
    float* armour = PedArmour(ped);
    const float armourBefore = *armour, healthBefore = PedHealth(ped);
    float rest = g_fallDamage;
    if (!g_noArmorDamage && armourBefore > 0.0f && rest > 0.0f)
    {
        const float take = (armourBefore < rest) ? armourBefore : rest;
        *armour = armourBefore - take;
        rest -= take;
    }
    uint8_t result = 0;
    float sent = 0.0f;
    if (rest > 0.0f)
    {
        // InflictDamage multiplies every hit on the player by 0.33 (found in the game code), so the value passed to it is divided by
        // that factor: this way exactly 'rest' points are taken from the health.
        const float mult = *(const float*)ADDR_PLAYER_DAMAGE_MULT;
        sent = (mult > 0.01f) ? rest / mult : rest;
        // The armor was already handled above, so it must not absorb anything in the game's call.
        const float armourNow = *armour;
        *armour = 0.0f;
        result = ((Inflict_t)FN_INFLICT_DAMAGE)(ped, damager, weapon, sent, piece, direction);
        *armour = armourNow;
    }
    g_applied = true;
    g_appliedTime = GameTimeMs();
    Log("%s: game damage %.2f replaced by FallDamage %.2f (NoArmorDamage=%d, value sent to the game %.2f). Armor %.1f -> %.1f, health %.1f -> %.1f.",
        source, gameDamage, g_fallDamage, g_noArmorDamage ? 1 : 0, sent, armourBefore, *armour, healthBefore, PedHealth(ped));
    return result;
}

// Replaces the call that applies the FALL damage on a hard landing (the game passes 15.0).
static uint8_t __fastcall Hook_FallDamage(void* ped, void* /*edx*/, void* damager, int weapon, float damage, int piece, int direction)
{
    if (g_enabled && weapon == WEAPON_FALL && ped && ped == PlayerPed())
    {
        if (AppliedRecently())
        {
            Log("Landing damage on the player (%.2f) skipped: this fall already got its damage.", damage);
            return 0;
        }
        const bool afterBikeFall = g_knocked && (uint32_t)(GameTimeMs() - g_knockTime) <= g_windowMs;
        if (!g_onlyAfterBikeFall || afterBikeFall)
            return ApplyFall(ped, damager, weapon, piece, direction, "Landing damage on the player", damage);
        Log("Landing damage on the player NOT changed (%.2f): not after a bike fall.", damage);
    }
    return g_origInflict(ped, damager, weapon, damage, piece, direction);
}

// Replaces the two FALL damage calls of the bike code that run while a bike is falling over / lying on its side (0x6137BD for the
// driver, 0x613887 for the passengers). The game passes CTimer::ms_fTimeStep (about 1 point) and repeats it every frame.
static uint8_t __fastcall Hook_TipDamage(void* ped, void* /*edx*/, void* damager, int weapon, float damage, int piece, int direction)
{
    if (g_enabled && weapon == WEAPON_FALL && ped && ped == PlayerPed())
    {
        if (AppliedRecently())
        {
            // The game would take about 1 point again on every frame of the fall: skip it.
            return 0;
        }
        return ApplyFall(ped, damager, weapon, piece, direction, "Bike tipping over, damage on the player", damage);
    }
    return ((Inflict_t)FN_INFLICT_DAMAGE)(ped, damager, weapon, damage, piece, direction);
}

// Replaces the two InflictDamage calls of CBike::VehicleDamage (crash damage to the rider). When the rider is the player and
// the game is about to knock him off the bike, the game's computed damage is replaced by FallDamage (see ApplyFall).
static uint8_t __fastcall Hook_CrashDamage(void* ped, void* /*edx*/, void* damager, int weapon, float damage, int piece, int direction)
{
    if (g_enabled && weapon == WEAPON_CRASH && ped && ped == PlayerPed()
        && *(const int32_t*)((const uint8_t*)ped + OFF_PED_STATE) == PED_STATE_DRIVING)
    {
        if (AppliedRecently())
        {
            Log("Bike crash damage on the player (%.2f) skipped: this fall already got its damage.", damage);
            return 0;
        }
        return ApplyFall(ped, damager, weapon, piece, direction, "Bike crash on the player", damage);
    }
    return ((Inflict_t)FN_INFLICT_DAMAGE)(ped, damager, weapon, damage, piece, direction);
}

// With EnableLog=1: samples the player's health every 100 ms for a few seconds after a knock-off, to see WHERE health is lost.
static DWORD WINAPI SamplerThread(LPVOID)
{
    float last = -1.0f; bool wasActive = false;
    for (;;)
    {
        Sleep(100);
        if (!g_logEnabled) continue;
        void* p = PlayerPed();
        if (!p) continue;
        const bool active = g_knocked && (uint32_t)(GameTimeMs() - g_knockTime) <= g_windowMs;
        const float h = PedHealth(p);
        if (active && wasActive && h != last) Log("  [health after knock-off] %.1f -> %.1f (t+%u ms)", last, h, (unsigned)(GameTimeMs() - g_knockTime));
        last = h; wasActive = active;
    }
    return 0;
}

// Swaps the target of a "call rel32" if it points to 'expected' and returns the original target (0 on failure)
static uintptr_t PatchCall(uintptr_t site, void* newTarget)
{
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return 0;
    const uintptr_t orig = site + 5 + (uintptr_t)(intptr_t)(*(int32_t*)(p + 1));
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)newTarget - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return orig;
}

static uintptr_t CallTarget(uintptr_t site)
{
    const uint8_t* p = (const uint8_t*)site;
    if (p[0] != 0xE8) return 0;
    return site + 5 + (uintptr_t)(intptr_t)(*(const int32_t*)(p + 1));
}

BOOL APIENTRY DllMain(HMODULE hm, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hm);

    GetModuleFileNameA(hm, g_iniPath, MAX_PATH);
    char* dot = strrchr(g_iniPath, '.');
    if (dot) strcpy(dot, ".ini");

    g_logEnabled = GetPrivateProfileIntA("Settings", "EnableLog", 0, g_iniPath) != 0;
    if (g_logEnabled) { FILE* f = fopen("BikeFallDamageVC.log", "w"); if (f) fclose(f); }

    if (*(const uint32_t*)VERSION_ADDR != VERSION_OK)
    {
        Log("Exe version not recognized (expected Vice City 1.0). Mod disabled.");
        return TRUE;
    }

    LoadConfig();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fad)) g_lastWrite = fad.ftLastWriteTime;

    // 1) The landing damage call. If another plugin already hooked it, we join its chain.
    const uintptr_t t = CallTarget(SITE_FALL_DAMAGE);
    if (!t) { Log("Landing call site %X is not a call. Mod disabled.", (unsigned)SITE_FALL_DAMAGE); return TRUE; }
    if (t != FN_INFLICT_DAMAGE) Log("Call site %X: target %X (expected %X) - another plugin already hooked it; joining its chain.", (unsigned)SITE_FALL_DAMAGE, (unsigned)t, (unsigned)FN_INFLICT_DAMAGE);
    const uintptr_t orig = PatchCall(SITE_FALL_DAMAGE, (void*)&Hook_FallDamage);
    if (!orig) { Log("Failed to hook the landing call. Mod disabled."); return TRUE; }
    g_origInflict = (Inflict_t)orig;

    // 2) The knock-off calls. A site whose target is not the game's function (hooked by another plugin) is skipped.
    int hooked = 0;
    for (size_t i = 0; i < sizeof(SITES_KNOCK_OFF) / sizeof(SITES_KNOCK_OFF[0]); ++i)
    {
        const uintptr_t s = SITES_KNOCK_OFF[i];
        if (CallTarget(s) != FN_KNOCK_OFF_RIDER) { Log("Knock-off site %X skipped (not a call to %X).", (unsigned)s, (unsigned)FN_KNOCK_OFF_RIDER); continue; }
        if (PatchCall(s, (void*)&Hook_KnockOff)) ++hooked;
    }

    // 3) The crash damage calls of CBike::VehicleDamage (only if they still point to the game's function).
    int crashHooked = 0;
    for (size_t i = 0; i < sizeof(SITES_CRASH_DAMAGE) / sizeof(SITES_CRASH_DAMAGE[0]); ++i)
    {
        const uintptr_t s = SITES_CRASH_DAMAGE[i];
        if (CallTarget(s) != FN_INFLICT_DAMAGE) { Log("Crash damage site %X skipped (not a call to %X).", (unsigned)s, (unsigned)FN_INFLICT_DAMAGE); continue; }
        if (PatchCall(s, (void*)&Hook_CrashDamage)) ++crashHooked;
    }

    // 4) The tipping-over damage calls of the bike code.
    int tipHooked = 0;
    for (size_t i = 0; i < sizeof(SITES_TIP_DAMAGE) / sizeof(SITES_TIP_DAMAGE[0]); ++i)
    {
        const uintptr_t s = SITES_TIP_DAMAGE[i];
        if (CallTarget(s) != FN_INFLICT_DAMAGE) { Log("Tip damage site %X skipped (not a call to %X).", (unsigned)s, (unsigned)FN_INFLICT_DAMAGE); continue; }
        if (PatchCall(s, (void*)&Hook_TipDamage)) ++tipHooked;
    }

    Log("OK: landing hook installed, %d knock-off sites, %d crash damage sites and %d tip damage sites hooked (FallDamage=%.2f, NoArmorDamage=%d, OnlyAfterBikeFall=%d, window=%lu ms).",
        hooked, crashHooked, tipHooked, g_fallDamage, g_noArmorDamage ? 1 : 0, g_onlyAfterBikeFall ? 1 : 0, (unsigned long)g_windowMs);
    if (g_logEnabled) CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
    return TRUE;
}
