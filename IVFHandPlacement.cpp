// IVFHandPlacement v3 - STAGE 3: both hands, two-bone IK, per-model settings.
// NOT TESTED IN GAME. Target: GTA SA 1.0 US.
#include "plugin.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CWorld.h"
#include "CTimer.h"
#include "CModelInfo.h"
#include "RenderWare.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>

using namespace plugin;

// ---------------- tiny vector math ----------------
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Len(V3 a) { return sqrtf(Dot(a, a)); }
static V3 Norm(V3 a) { float l = Len(a); if (l < 1e-6f) return { 0, 0, 0 }; return a * (1.0f / l); }
static V3 FromRw(const RwV3d& v) { return { v.x, v.y, v.z }; }
static void ToRw(RwV3d& d, V3 s) { d.x = s.x; d.y = s.y; d.z = s.z; }

// ---------------- settings ----------------
struct Cfg {
    bool enabled = false, absolute = false, left = true, right = true;
    V3 l{ 0, 0, 0 }, r{ 0, 0, 0 };
};
static std::map<int, Cfg> g_cfg;
static bool g_enabled = true;
static int  g_toggleKey = VK_F9;
static int  g_logMs = 2000;
static std::string g_logPath, g_iniPath;

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_logPath.c_str(), "a");
    if (!f) return;
    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fputc('\n', f);
    fclose(f);
}

static float ReadF(const char* sec, const char* key, float def) {
    char b[32] = {};
    GetPrivateProfileStringA(sec, key, "", b, sizeof(b), g_iniPath.c_str());
    return b[0] ? (float)atof(b) : def;
}

static void LoadIni() {
    const char* ini = g_iniPath.c_str();
    g_enabled = GetPrivateProfileIntA("Settings", "Enabled", 1, ini) != 0;
    g_toggleKey = GetPrivateProfileIntA("Settings", "ToggleKey", VK_F9, ini);
    g_logMs = GetPrivateProfileIntA("Settings", "LogEveryMs", 2000, ini);
    g_cfg.clear();
    char names[4096] = {};
    GetPrivateProfileSectionNamesA(names, sizeof(names), ini);
    for (const char* p = names; *p; p += strlen(p) + 1) {
        bool digits = true;
        for (const char* q = p; *q; ++q) if (*q < '0' || *q > '9') digits = false;
        if (!digits) continue;               // only [<model id>] sections for now
        int id = atoi(p);
        Cfg c;
        c.enabled = GetPrivateProfileIntA(p, "Enabled", 0, ini) != 0;
        char m[16] = {};
        GetPrivateProfileStringA(p, "Mode", "Offset", m, sizeof(m), ini);
        c.absolute = _stricmp(m, "Absolute") == 0;
        c.left = GetPrivateProfileIntA(p, "Left", 1, ini) != 0;
        c.right = GetPrivateProfileIntA(p, "Right", 1, ini) != 0;
        c.l = { ReadF(p, "LeftHandX", 0), ReadF(p, "LeftHandY", 0), ReadF(p, "LeftHandZ", 0) };
        c.r = { ReadF(p, "RightHandX", 0), ReadF(p, "RightHandY", 0), ReadF(p, "RightHandZ", 0) };
        g_cfg[id] = c;
        Log("ini: model %d enabled=%d mode=%s L=%d R=%d", id, c.enabled, c.absolute ? "Absolute" : "Offset", c.left, c.right);
    }
    Log("ini: global enabled=%d, %d model section(s)", g_enabled, (int)g_cfg.size());
}

// ---------------- rotation of a part of the skeleton ----------------
static V3 RotVec(V3 v, V3 axis, float c, float s) {
    return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1.0f - c));
}

static bool RotateFromTo(RwMatrix* arr, int from, int to, V3 pivot, V3 a, V3 b) {
    a = Norm(a); b = Norm(b);
    V3 ax = Cross(a, b);
    float s = Len(ax), c = Dot(a, b);
    if (s < 1e-5f) return false;
    ax = ax * (1.0f / s);
    for (int i = from; i <= to; i++) {
        V3 p = FromRw(arr[i].pos);
        ToRw(arr[i].pos, pivot + RotVec(p - pivot, ax, c, s));
        ToRw(arr[i].right, RotVec(FromRw(arr[i].right), ax, c, s));
        ToRw(arr[i].up, RotVec(FromRw(arr[i].up), ax, c, s));
        ToRw(arr[i].at, RotVec(FromRw(arr[i].at), ax, c, s));
    }
    return true;
}

// Two-bone IK: put the wrist at 'target' (world space) by bending the elbow.
static bool SolveArm(RpHAnimHierarchy* h, RwMatrix* arr, int upperId, int foreId, int handId, V3 target) {
    int iu = RpHAnimIDGetIndex(h, upperId);
    int ie = RpHAnimIDGetIndex(h, foreId);
    int ih = RpHAnimIDGetIndex(h, handId);
    if (iu < 0 || ie != iu + 1 || ih != iu + 2) return false;
    int n = (int)h->numNodes, end = iu;
    while (end < n - 1 && !(h->pNodeInfo[end].flags & 1)) end++;   // chain ends at the node with POP flag

    V3 S = FromRw(arr[iu].pos), E = FromRw(arr[ie].pos), W = FromRw(arr[ih].pos);
    float L1 = Len(E - S), L2 = Len(W - E);
    if (L1 < 0.01f || L2 < 0.01f) return false;

    V3 d = target - S;
    float D = Len(d);
    if (D < 1e-4f) return false;
    V3 dir = d * (1.0f / D);
    float maxD = (L1 + L2) * 0.999f, minD = fabsf(L1 - L2) + 0.01f;
    if (D > maxD) D = maxD;
    if (D < minD) D = minD;
    V3 tw = S + dir * D;                       // reachable wrist position

    V3 ev = E - S;                             // keep the natural elbow side
    V3 pole = ev - dir * Dot(ev, dir);
    if (Len(pole) < 1e-4f) { V3 down = { 0, 0, -1 }; pole = down - dir * Dot(down, dir); }
    pole = Norm(pole);

    float a = (L1 * L1 - L2 * L2 + D * D) / (2.0f * D);
    float hh = L1 * L1 - a * a; if (hh < 0) hh = 0; hh = sqrtf(hh);
    V3 E2 = S + dir * a + pole * hh;

    RotateFromTo(arr, iu, end, S, E - S, E2 - S);              // shoulder
    V3 E1 = FromRw(arr[ie].pos), W1 = FromRw(arr[ih].pos);
    RotateFromTo(arr, ie, end, E1, W1 - E1, tw - E1);          // elbow
    return true;
}

// ---------------- vehicle space ----------------
struct Basis { V3 pos, r, f, u; };   // right, forward, up
static bool GetBasis(CVehicle* v, Basis& b) {
    if (!v || !v->m_pRwClump) return false;
    RwFrame* fr = (RwFrame*)v->m_pRwClump->object.parent;
    if (!fr) return false;
    RwMatrix* m = &fr->ltm;
    b.pos = FromRw(m->pos);
    b.r = Norm(FromRw(m->right));
    b.f = Norm(FromRw(m->up));      // RW "up" axis = vehicle forward in GTA
    b.u = Norm(FromRw(m->at));      // RW "at" axis = vehicle up
    return true;
}
static V3 ToLocal(const Basis& b, V3 p) { V3 d = p - b.pos; return { Dot(d, b.r), Dot(d, b.f), Dot(d, b.u) }; }
static V3 ToWorld(const Basis& b, V3 l) { return b.pos + b.r * l.x + b.f * l.y + b.u * l.z; }

// ---------------- main logic ----------------
static RpHAnimHierarchy* g_hier = nullptr;
static std::vector<RwMatrix> g_backup;

static bool IsPlayerDriving(CPed* ped) {
    if (!ped) return false;
    CVehicle* veh = ped->m_pVehicle;
    return veh && veh->m_pDriver == ped;
}

static void BeforePedRender(CPed* ped) {
    static DWORD nextLog = 0;
    static bool errOnce = false;
    if (!g_enabled || ped != FindPlayerPed(0)) return;
    if (!IsPlayerDriving(ped) || !ped->m_pRwClump) return;
    CVehicle* veh = ped->m_pVehicle;

    RpHAnimHierarchy* hier = GetAnimHierarchyFromSkinClump(ped->m_pRwClump);
    if (!hier) { if (!errOnce) { Log("ERROR: no anim hierarchy"); errOnce = true; } return; }
    RwMatrix* arr = RpHAnimHierarchyGetMatrixArray(hier);
    if (!arr) return;
    int n = (int)hier->numNodes;

    // If this ped was already modified in this frame (several render passes), start from the original pose.
    if (g_hier == hier && (int)g_backup.size() == n) {
        for (int i = 0; i < n; i++) arr[i] = g_backup[i];
    }

    Basis vb;
    bool haveBasis = GetBasis(veh, vb);
    int iL = RpHAnimIDGetIndex(hier, 34), iR = RpHAnimIDGetIndex(hier, 24);

    // diagnostics: where the standard animation puts the hands (vehicle space)
    DWORD now = GetTickCount();
    if (haveBasis && iL >= 0 && iR >= 0 && now >= nextLog) {
        nextLog = now + (DWORD)g_logMs;
        V3 L = ToLocal(vb, FromRw(arr[iL].pos)), R = ToLocal(vb, FromRw(arr[iR].pos));
        Log("model=%d steer=%.3f | std hands (vehicle space) L=%.3f %.3f %.3f  R=%.3f %.3f %.3f",
            (int)veh->m_nModelIndex, veh->m_fSteerAngle, L.x, L.y, L.z, R.x, R.y, R.z);
    }

    auto it = g_cfg.find((int)veh->m_nModelIndex);
    if (it == g_cfg.end() || !it->second.enabled || !haveBasis) return;
    const Cfg& c = it->second;

    g_hier = hier;
    g_backup.assign(arr, arr + n);

    if (c.left && iL >= 0) {
        V3 cur = ToLocal(vb, FromRw(arr[iL].pos));
        V3 tgt = c.absolute ? c.l : cur + c.l;
        if (!SolveArm(hier, arr, 32, 33, 34, ToWorld(vb, tgt)) && !errOnce) { Log("WARN: left arm solve failed"); errOnce = true; }
    }
    if (c.right && iR >= 0) {
        V3 cur = ToLocal(vb, FromRw(arr[iR].pos));
        V3 tgt = c.absolute ? c.r : cur + c.r;
        if (!SolveArm(hier, arr, 22, 23, 24, ToWorld(vb, tgt)) && !errOnce) { Log("WARN: right arm solve failed"); errOnce = true; }
    }
}

static void AfterFrame() {
    // put the original pose back so nothing accumulates between frames
    if (g_hier && !g_backup.empty()) {
        CPed* p = FindPlayerPed(0);
        if (p && p->m_pRwClump && GetAnimHierarchyFromSkinClump(p->m_pRwClump) == g_hier) {
            RwMatrix* arr = RpHAnimHierarchyGetMatrixArray(g_hier);
            if (arr && (int)g_backup.size() == (int)g_hier->numNodes)
                for (size_t i = 0; i < g_backup.size(); i++) arr[i] = g_backup[i];
        }
    }
    g_hier = nullptr;
    g_backup.clear();

    static bool keyWas = false;
    bool keyNow = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    if (keyNow && !keyWas) { g_enabled = !g_enabled; Log("toggle: enabled=%d", g_enabled); }
    keyWas = keyNow;
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
        Log("---- IVFHandPlacement v3 (stage 3) started ----");
        LoadIni();
        Events::pedRenderEvent += BeforePedRender;
        Events::drawingEvent += []() { AfterFrame(); };
    }
} g_instance;
