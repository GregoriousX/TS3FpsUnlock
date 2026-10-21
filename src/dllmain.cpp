// TS3FpsUnlock.asi - raises the frame cap of Toy Story 3 Complete Edition (PC, Steam)
//
// The game runs on a variable time step (CMasterTimer g_timer, dt in ms) and the
// physics splits dt into sub-steps of at most 20 ms, so a higher frame rate does
// not change gameplay. The 60 FPS cap comes from two places:
//   FrameLimiter()            -> busy-wait to 1/60 s (or 1/30 s with "Frame cap 30")
//   IDXGISwapChain::Present   -> SyncInterval = 1 (VSync)
// This plugin replaces the limiter with a waitable-timer limiter at TargetFPS,
// optionally presents with SyncInterval 0, and can optionally feed the game a
// sub-millisecond dt (CMasterTimer::NewFrame).
// Everything is found by signature scan at runtime; if a signature does not match,
// that part stays off and the reason goes to TS3FpsUnlock.log.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using NewFrame_t = void(__fastcall*)(uint8_t* timer);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static uint8_t* g_base;
static size_t   g_size;
static char     g_iniPath[MAX_PATH];
static char     g_logPath[MAX_PATH];

// Config
static int  g_targetFps   = 120;
static bool g_vsync       = true;
static bool g_preciseDt   = false;
static bool g_logStats    = false;

static uint32_t* g_settingsFlags;        // bit 1 = "Frame cap 30" menu option
static NewFrame_t g_NewFrameOrig;        // trampoline

// Limiter state (main thread only)
static LARGE_INTEGER g_freq;
static HANDLE  g_waitTimer;
static bool    g_highResTimer;
static int64_t g_next;                   // QPC deadline of the next frame
static int     g_lastFps;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Signature scanning (over .text) and patching
// ---------------------------------------------------------------------------
static uint8_t* g_textBegin;
static uint8_t* g_textEnd;

// Returns the match only if the pattern occurs exactly once in .text.
static uint8_t* FindPattern(const char* pattern)
{
    uint8_t bytes[128];
    bool    mask[128];
    int     n = 0;
    for (const char* p = pattern; *p && n < 128;)
    {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { mask[n] = false; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { p[0], p[1], 0 };
        bytes[n] = (uint8_t)strtoul(hex, nullptr, 16);
        mask[n++] = true;
        p += 2;
    }
    uint8_t* found = nullptr;
    for (uint8_t* s = g_textBegin; s + n <= g_textEnd; ++s)
    {
        int i = 0;
        while (i < n && (!mask[i] || s[i] == bytes[i])) ++i;
        if (i == n)
        {
            if (found) return nullptr;    // not unique
            found = s;
        }
    }
    return found;
}

static uint8_t* Rip(uint8_t* insn, int dispOffset, int insnLen)
{
    return insn + insnLen + *reinterpret_cast<int32_t*>(insn + dispOffset);
}

static bool Patch(uint8_t* target, const void* bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(target, bytes, len);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return true;
}

// 14-byte absolute jmp to 'detour'; with stolen > 0 also builds a trampoline
// that runs the stolen bytes and jumps back.
static void* HookEntry(uint8_t* target, size_t stolen, void* detour)
{
    uint8_t* tramp = nullptr;
    if (stolen)
    {
        tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tramp) return nullptr;
        memcpy(tramp, target, stolen);
        uint8_t jmpBack[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
        *reinterpret_cast<uint64_t*>(jmpBack + 6) = (uint64_t)(target + stolen);
        memcpy(tramp + stolen, jmpBack, sizeof(jmpBack));
    }
    uint8_t jmp[32] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *reinterpret_cast<uint64_t*>(jmp + 6) = (uint64_t)detour;
    size_t len = stolen > 14 ? stolen : 14;
    for (size_t i = 14; i < len; ++i) jmp[i] = 0x90;
    if (!Patch(target, jmp, len)) return nullptr;
    return tramp ? tramp : target;
}

// ---------------------------------------------------------------------------
// Frame limiter
// ---------------------------------------------------------------------------
static int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void WaitUntil(int64_t deadline)
{
    const int64_t spin = g_freq.QuadPart / 1000;            // last ~1 ms is spun
    for (;;)
    {
        int64_t left = deadline - Now();
        if (left <= spin) break;
        int64_t sleepTicks = left - spin;
        if (g_waitTimer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(sleepTicks * 10000000 / g_freq.QuadPart);   // 100 ns units, relative
            if (due.QuadPart >= 0) break;
            if (!SetWaitableTimerEx(g_waitTimer, &due, 0, nullptr, nullptr, nullptr, 0)) { g_waitTimer = nullptr; timeBeginPeriod(1); continue; }
            WaitForSingleObject(g_waitTimer, INFINITE);
            if (g_highResTimer) break;                       // precise enough, spin the rest
        }
        else
        {
            Sleep(1);                                        // timeBeginPeriod(1) is active
        }
    }
    while (Now() < deadline) _mm_pause();
}

static void LogStats(int64_t now)
{
    static int64_t s_start;
    static int     s_frames;
    if (!s_start) { s_start = now; s_frames = 0; return; }
    ++s_frames;
    int64_t span = now - s_start;
    if (span >= g_freq.QuadPart * 10)
    {
        Log("stats: %.1f FPS over %.1f s (cap %d)", s_frames * (double)g_freq.QuadPart / span,
            span / (double)g_freq.QuadPart, g_lastFps);
        s_start = now;
        s_frames = 0;
    }
}

// Replaces the game's FrameLimiter(); called once per frame on the main thread.
static void __fastcall FrameLimiter_Hook()
{
    int fps = (*g_settingsFlags & 2) ? 30 : g_targetFps;
    int64_t period = g_freq.QuadPart / fps;
    int64_t now = Now();

    if (fps != g_lastFps)
    {
        Log("frame cap -> %d FPS", fps);
        g_lastFps = fps;
        g_next = now;
    }

    if (now < g_next)
    {
        WaitUntil(g_next);
        g_next += period;
    }
    else if (now - g_next > period)
        g_next = now + period;                               // fell behind: don't catch up with short frames
    else
        g_next += period;

    if (g_logStats) LogStats(Now());
}

// ---------------------------------------------------------------------------
// Precise delta time (experimental)
// ---------------------------------------------------------------------------
// CMasterTimer layout: +0x04 real dt [ms], +0x10 game dt [ms], +0x1C game dt [s],
// +0x28 smoothed dt [s], +0x2C/+0x30 smoothed time, +0x34/+0x38 smoothing weights.
static void __fastcall NewFrame_Hook(uint8_t* timer)
{
    static int64_t s_last;
    int64_t now = Now();
    float oldSmooth = *reinterpret_cast<float*>(timer + 0x28);

    g_NewFrameOrig(timer);

    int64_t last = s_last;
    s_last = now;
    if (!last) return;

    uint32_t realMs = *reinterpret_cast<uint32_t*>(timer + 0x04);
    uint32_t gameMs = *reinterpret_cast<uint32_t*>(timer + 0x10);
    if (!realMs || !gameMs) return;

    double real = (double)(now - last) / g_freq.QuadPart;
    // Only refine whole-ms values; anything unusual (clamps, frozen clock) keeps the game's value.
    if (real * 1000.0 < realMs - 1.5 || real * 1000.0 > realMs + 1.5) return;

    float dt = (float)(real * gameMs / realMs);
    *reinterpret_cast<float*>(timer + 0x1C) = dt;
    float smooth = *reinterpret_cast<float*>(timer + 0x34) * dt + *reinterpret_cast<float*>(timer + 0x38) * oldSmooth;
    *reinterpret_cast<float*>(timer + 0x28) = smooth;
    *reinterpret_cast<float*>(timer + 0x30) = *reinterpret_cast<float*>(timer + 0x2C) + smooth;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    g_targetFps = GetPrivateProfileIntA("FPS", "TargetFPS", 120, g_iniPath);
    if (g_targetFps < 30) g_targetFps = 30;
    if (g_targetFps > 360) g_targetFps = 360;
    g_vsync     = GetPrivateProfileIntA("FPS", "VSync", 1, g_iniPath) != 0;
    g_preciseDt = GetPrivateProfileIntA("FPS", "PreciseDeltaTime", 0, g_iniPath) != 0;
    g_logStats  = GetPrivateProfileIntA("FPS", "LogStats", 0, g_iniPath) != 0;
}

static bool Init()
{
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (memcmp(sec->Name, ".text", 6) == 0)
        {
            g_textBegin = g_base + sec->VirtualAddress;
            g_textEnd   = g_textBegin + sec->Misc.VirtualSize;
        }
    if (!g_textBegin) { Log("no .text section, disabled"); return false; }

    QueryPerformanceFrequency(&g_freq);

    // Frame cap 30 getter: mov eax,[flags]; shr eax,1; and al,1; ret
    uint8_t* is30 = FindPattern("8B 05 ?? ?? ?? ?? D1 E8 24 01 C3");
    // FrameLimiter(): static timer init, then calls the getter and loads 1/30 or 1/60
    uint8_t* limiter = FindPattern("40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA ?? ?? ?? ?? "
                                   "48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F ?? E8 ?? ?? ?? ?? 84 C0 74 ?? F2 0F 10 0D");
    if (!is30 || !limiter || Rip(limiter + 0x29, 1, 5) != is30)
    {
        Log("frame limiter not found (getter %p, limiter %p), unsupported game version, disabled", is30, limiter);
        return false;
    }
    g_settingsFlags = reinterpret_cast<uint32_t*>(Rip(is30, 2, 6));

    g_waitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    g_highResTimer = g_waitTimer != nullptr;
    if (!g_waitTimer) timeBeginPeriod(1);

    if (!HookEntry(limiter, 0, reinterpret_cast<void*>(FrameLimiter_Hook)))
    {
        Log("could not patch the frame limiter, disabled");
        return false;
    }
    Log("frame limiter replaced: TargetFPS %d (menu \"Frame cap 30\" still gives 30), wait: %s",
        g_targetFps, g_highResTimer ? "high-resolution waitable timer + spin" : "Sleep(1) + spin");

    // Present: mov rcx,[rsi+618h]; mov rax,[rcx]; xor r8d,r8d; mov edx,[rsi+608h]; call [rax+40h]
    if (!g_vsync)
    {
        uint8_t* present = FindPattern("48 8B 8E 18 06 00 00 48 8B 01 45 33 C0 8B 96 08 06 00 00 FF 50 40");
        static const uint8_t movEdx0[6] = { 0xBA, 0x00, 0x00, 0x00, 0x00, 0x90 };   // mov edx,0; nop
        if (present && Patch(present + 0x0D, movEdx0, sizeof(movEdx0)))
            Log("VSync off: Present uses SyncInterval 0");
        else
            Log("Present call not found, VSync stays on");
    }
    else
        Log("VSync on (game default): the frame rate is also limited by the monitor refresh rate");

    if (g_preciseDt)
    {
        // CMasterTimer::NewFrame: push rbx; sub rsp,20h; mov eax,[rcx+0Ch]; mov rbx,rcx; mov [rcx+8],eax; call Time
        uint8_t* nf = FindPattern("40 53 48 83 EC 20 8B 41 0C 48 8B D9 89 41 08 E8 ?? ?? ?? ?? 8B 4B 08 89 43 0C 3B C1 77");
        if (nf)
        {
            g_NewFrameOrig = reinterpret_cast<NewFrame_t>(HookEntry(nf, 15, reinterpret_cast<void*>(NewFrame_Hook)));
            Log(g_NewFrameOrig ? "PreciseDeltaTime on (experimental)" : "PreciseDeltaTime: hook failed");
        }
        else
            Log("PreciseDeltaTime: CMasterTimer::NewFrame not found, off");
    }
    return true;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        GetModuleFileNameA(mod, g_iniPath, MAX_PATH);
        strcpy_s(g_logPath, g_iniPath);
        if (char* dot = strrchr(g_iniPath, '.')) strcpy_s(dot, MAX_PATH - (dot - g_iniPath), ".ini");
        if (char* dot = strrchr(g_logPath, '.')) strcpy_s(dot, MAX_PATH - (dot - g_logPath), ".log");
        DeleteFileA(g_logPath);
        if (GetPrivateProfileIntA("FPS", "Enabled", 1, g_iniPath) == 0) { Log("Enabled=0, doing nothing"); return TRUE; }
        LoadConfig();
        Init();
    }
    return TRUE;
}
