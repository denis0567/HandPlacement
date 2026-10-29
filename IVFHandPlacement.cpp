// IVFHandPlacement - STAGE 2 PROTOTYPE (one hand, test rotation)
// Goal of this stage: PROVE that an ASI can change a hand bone of the player
// while driving, at render time, without being overwritten by the animation.
//
// STATUS: NOT COMPILED, NOT TESTED IN GAME. Written against plugin-sdk (plugin_sa).
// If something fails to compile, send the exact error text.
//
// Target game: GTA SA 1.0 US (Hoodlum). Build: Release | Win32, output *.asi

#include "plugin.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CWorld.h"
#include "CTimer.h"
#include "CModelInfo.h"
#include "RenderWare.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>

using namespace plugin;

// ---------- settings (IVFHandPlacement.ini, section [Test]) ----------
static bool  g_enabled   = true;   // Enabled=1
static bool  g_leftHand  = true;   // Hand=Left / Right
static float g_angleDeg  = 40.0f;  // AngleDeg: rotation around world Z axis
static int   g_boneId    = 34;     // BoneId: 34 = left hand, 24 = right hand (SA bone ids, VERIFY in log)
static int   g_toggleKey = VK_F9;  // ToggleKey

static std::string g_logPath;
static std::string g_iniPath;

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fputc('\n', f);
    fclose(f);
}

static void LoadIni() {
    // No ini or bad values -> defaults, never crash.
    g_enabled = GetPrivateProfileIntA("Test", "Enabled", 1, g_iniPath.c_str()) != 0;
    char buf[32] = {};
    GetPrivateProfileStringA("Test", "Hand", "Left", buf, sizeof(buf), g_iniPath.c_str());
    g_leftHand = (_stricmp(buf, "Right") != 0);
    GetPrivateProfileStringA("Test", "AngleDeg", "40", buf, sizeof(buf), g_iniPath.c_str());
    g_angleDeg = (float)atof(buf);
    g_boneId = GetPrivateProfileIntA("Test", "BoneId", g_leftHand ? 34 : 24, g_iniPath.c_str());
    g_toggleKey = GetPrivateProfileIntA("Test", "ToggleKey", VK_F9, g_iniPath.c_str());
    Log("ini: enabled=%d hand=%s angle=%.1f boneId=%d", g_enabled, g_leftHand ? "Left" : "Right", g_angleDeg, g_boneId);
}

// ---------- saved matrix so we can restore after render ----------
static RpHAnimHierarchy* g_savedHier = nullptr;
static int               g_savedIdx  = -1;
static RwMatrix          g_savedMat;
static bool              g_hasSaved  = false;

static bool IsPlayerDriving(CPed* ped) {
    if (!ped) return false;
    CVehicle* veh = ped->m_pVehicle;
    return veh && veh->m_pDriver == ped;
}

// Rotate the bone matrix around its own position by angle about world Z.
static void RotateBoneZ(RwMatrix* m, float deg) {
    const float a = deg * 3.14159265f / 180.0f;
    const float c = cosf(a), s = sinf(a);
    auto rot = [&](RwV3d& v) {
        float x = v.x * c - v.y * s;
        float y = v.x * s + v.y * c;
        v.x = x; v.y = y;
    };
    rot(m->right); rot(m->up); rot(m->at);
    // pos unchanged on purpose
}

static void BeforePedRender(CPed* ped) {
    static bool loggedOnce = false;
    if (!g_enabled || ped != FindPlayerPed(0)) return;
    if (!IsPlayerDriving(ped)) return;
    if (!ped->m_pRwClump) return;

    RpHAnimHierarchy* hier = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!hier) {
        if (!loggedOnce) { Log("ERROR: no anim hierarchy for player clump"); loggedOnce = true; }
        return;
    }
    int idx = RpHAnimIDGetIndex(hier, g_boneId);
    if (idx < 0) {
        if (!loggedOnce) { Log("ERROR: bone id %d not found in hierarchy (numNodes=%d)", g_boneId, (int)hier->numNodes); loggedOnce = true; }
        return;
    }
    RwMatrix* arr = RpHAnimHierarchyGetMatrixArray(hier);
    if (!arr) return;

    g_savedHier = hier; g_savedIdx = idx; g_savedMat = arr[idx]; g_hasSaved = true;
    RotateBoneZ(&arr[idx], g_angleDeg);

    if (!loggedOnce) {
        Log("OK: modified bone id %d (index %d of %d) at render time", g_boneId, idx, (int)hier->numNodes);
        loggedOnce = true;
    }
}

static void RestoreAfterFrame() {
    // Put the original matrix back so we never accumulate if the animation
    // is not recomputed on some frame.
    if (!g_hasSaved) return;
    CPed* p = FindPlayerPed(0);
    if (p && p->m_pRwClump && GetAnimHierarchyFromSkinClump(p->m_pRwClump) == g_savedHier) {
        RwMatrix* arr = RpHAnimHierarchyGetMatrixArray(g_savedHier);
        if (arr) arr[g_savedIdx] = g_savedMat;
    }
    g_hasSaved = false;
}

static void PerFrameDiagnostics() {
    static DWORD nextLog = 0;
    static bool  keyWas = false;

    bool keyNow = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    if (keyNow && !keyWas) {
        g_enabled = !g_enabled;
        Log("toggle: enabled=%d", g_enabled);
    }
    keyWas = keyNow;

    // Every 2 s while driving: model id and steering angle (needed for stage 4).
    DWORD now = GetTickCount();
    if (now < nextLog) return;
    CPed* p = FindPlayerPed(0);
    if (IsPlayerDriving(p)) {
        CVehicle* v = p->m_pVehicle;
        Log("driving: modelIndex=%d steerAngle=%.3f", (int)v->m_nModelIndex, v->m_fSteerAngle);
        nextLog = now + 2000;
    }
}

class IVFHandPlacement {
public:
    IVFHandPlacement() {
        char path[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&BeforePedRender, &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string base(path);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.resize(dot);
        g_iniPath = base + ".ini";
        g_logPath = base + ".log";

        Log("---- IVFHandPlacement prototype started ----");
        LoadIni();

        // Before ped render: modify matrix. After frame (2D drawing is after 3D): restore.
        Events::pedRenderEvent += BeforePedRender;
        Events::drawingEvent += []() { RestoreAfterFrame(); PerFrameDiagnostics(); };
    }
} g_instance;
