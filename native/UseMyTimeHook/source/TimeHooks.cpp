/*
 * UseMyTime - TimeHooks 实现
 *
 * 虚拟时间模型（锚点 + 偏移 + 倍速），保证：
 *   1. SetOffsetMs  -> 虚拟时间立即跳到 (真实时间 + offset)，之后连续推进
 *   2. SetSpeed     -> 只改变推进速度，无时间跳变
 *   3. Reset        -> 平滑回到真实时间
 *   4. Shutdown     -> 卸载 hook，进程恢复读取真实系统时间
 */
#include "TimeHooks.h"

#include <cmath>

namespace usemytime {

// ---------------------------------------------------------------------------
// 静态成员定义
// ---------------------------------------------------------------------------
std::atomic<long long> TimeHooks::s_anchorTickMs{ 0 };
std::atomic<long long> TimeHooks::s_anchorAbsMs{ 0 };
std::atomic<long long> TimeHooks::s_offsetMs{ 0 };
std::atomic<double>    TimeHooks::s_speed{ 1.0 };
std::atomic<bool>      TimeHooks::s_enabled{ false };

FnGetSystemTimeAsFileTime TimeHooks::s_origGetSystemTimeAsFileTime = nullptr;
FnGetSystemTime           TimeHooks::s_origGetSystemTime = nullptr;
FnGetLocalTime            TimeHooks::s_origGetLocalTime = nullptr;
FnGetTickCount64          TimeHooks::s_origGetTickCount64 = nullptr;
FnGetFileTime             TimeHooks::s_origGetFileTime = nullptr;

void* TimeHooks::s_hookGetSystemTimeAsFileTime =
    reinterpret_cast<void*>(&HookedGetSystemTimeAsFileTime);
void* TimeHooks::s_hookGetSystemTime =
    reinterpret_cast<void*>(&HookedGetSystemTime);
void* TimeHooks::s_hookGetLocalTime =
    reinterpret_cast<void*>(&HookedGetLocalTime);
void* TimeHooks::s_hookGetTickCount64 =
    reinterpret_cast<void*>(&HookedGetTickCount64);
void* TimeHooks::s_hookGetFileTime =
    reinterpret_cast<void*>(&HookedGetFileTime);

HMODULE     TimeHooks::s_module = nullptr;
std::mutex  TimeHooks::s_initMutex;

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
namespace {

// FILETIME (1601 纪元, 100ns) <-> 毫秒
long long FileTimeToMs(const FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.LowPart  = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(v.QuadPart) / 10000LL;
}

void MsToFileTime(long long ms, FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.QuadPart = ms * 10000LL;
    ft.dwLowDateTime  = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
}

// 将 FILETIME 增加 deltaMs（防溢出）
void AddDeltaToFileTime(FILETIME& ft, long long deltaMs)
{
    if (deltaMs == 0) return;
    long long ms = FileTimeToMs(ft);
    ms += deltaMs;
    if (ms < 0) ms = 0;
    MsToFileTime(ms, ft);
}

} // namespace

// ---------------------------------------------------------------------------
// 初始化 / 关闭
// ---------------------------------------------------------------------------
bool TimeHooks::Initialize()
{
    std::lock_guard<std::mutex> lk(s_initMutex);

    if (s_enabled.load(std::memory_order_relaxed)) return true; // 幂等

    // MinHook 全局初始化（幂等）
    if (MH_Initialize() != MH_OK) return false;

    // 解析 kernel32 原始函数
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return false;

    s_origGetSystemTimeAsFileTime =
        reinterpret_cast<FnGetSystemTimeAsFileTime>(
            GetProcAddress(k32, "GetSystemTimeAsFileTime"));
    s_origGetSystemTime =
        reinterpret_cast<FnGetSystemTime>(
            GetProcAddress(k32, "GetSystemTime"));
    s_origGetLocalTime =
        reinterpret_cast<FnGetLocalTime>(
            GetProcAddress(k32, "GetLocalTime"));
    s_origGetTickCount64 =
        reinterpret_cast<FnGetTickCount64>(
            GetProcAddress(k32, "GetTickCount64"));
    s_origGetFileTime =
        reinterpret_cast<FnGetFileTime>(
            GetProcAddress(k32, "GetFileTime"));

    if (!s_origGetSystemTimeAsFileTime || !s_origGetSystemTime ||
        !s_origGetLocalTime || !s_origGetTickCount64 || !s_origGetFileTime)
        return false;

    // 建立 5 个 hook（MH_CreateHook 创建，MH_EnableHook 激活）
    if (MH_CreateHook(s_origGetSystemTimeAsFileTime,
                      s_hookGetSystemTimeAsFileTime, nullptr) != MH_OK)
        return false;
    if (MH_CreateHook(s_origGetSystemTime,
                      s_hookGetSystemTime, nullptr) != MH_OK)
        return false;
    if (MH_CreateHook(s_origGetLocalTime,
                      s_hookGetLocalTime, nullptr) != MH_OK)
        return false;
    if (MH_CreateHook(s_origGetTickCount64,
                      s_hookGetTickCount64, nullptr) != MH_OK)
        return false;
    if (MH_CreateHook(s_origGetFileTime,
                      s_hookGetFileTime, nullptr) != MH_OK)
        return false;

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
        return false;

    // 设置锚点 = 当前真实时间（delta 从 0 开始）
    ULONGLONG realTick = s_origGetTickCount64();
    FILETIME  ft;
    s_origGetSystemTimeAsFileTime(&ft);

    s_anchorTickMs.store(static_cast<long long>(realTick),
                         std::memory_order_relaxed);
    s_anchorAbsMs.store(FileTimeToMs(ft), std::memory_order_relaxed);
    s_offsetMs.store(0, std::memory_order_relaxed);
    s_speed.store(1.0, std::memory_order_relaxed);

    s_enabled.store(true, std::memory_order_release);
    return true;
}

void TimeHooks::Shutdown()
{
    std::lock_guard<std::mutex> lk(s_initMutex);

    if (!s_enabled.load(std::memory_order_relaxed)) return; // 幂等

    // 先标记失效（新调用立即走真实时间），再逐个卸载
    s_enabled.store(false, std::memory_order_release);

    MH_DisableHook(reinterpret_cast<VOID*>(s_origGetSystemTimeAsFileTime));
    MH_DisableHook(reinterpret_cast<VOID*>(s_origGetSystemTime));
    MH_DisableHook(reinterpret_cast<VOID*>(s_origGetLocalTime));
    MH_DisableHook(reinterpret_cast<VOID*>(s_origGetTickCount64));
    MH_DisableHook(reinterpret_cast<VOID*>(s_origGetFileTime));

    s_origGetSystemTimeAsFileTime = nullptr;
    s_origGetSystemTime = nullptr;
    s_origGetLocalTime = nullptr;
    s_origGetTickCount64 = nullptr;
    s_origGetFileTime = nullptr;

    s_offsetMs.store(0, std::memory_order_relaxed);
    s_speed.store(1.0, std::memory_order_relaxed);
}

bool TimeHooks::IsEnabled()
{
    return s_enabled.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// 状态控制
// ---------------------------------------------------------------------------
void TimeHooks::SetOffsetMs(long long offsetMs)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;

    // 重新锚定：保证 delta 连续（虚拟时间立即变为 real+offset）
    long long realTick = static_cast<long long>(s_origGetTickCount64());
    FILETIME  ft;
    s_origGetSystemTimeAsFileTime(&ft);

    s_anchorTickMs.store(realTick, std::memory_order_relaxed);
    s_anchorAbsMs.store(FileTimeToMs(ft), std::memory_order_relaxed);
    s_offsetMs.store(offsetMs, std::memory_order_relaxed);
}

void TimeHooks::SetSpeed(double speed)
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;
    if (speed < 0.0) speed = 0.0;
    if (speed > 1000.0) speed = 1000.0; // 上限保护

    // 重新锚定：保持当前虚拟时间不变，只改变后续流速
    long long realTick = static_cast<long long>(s_origGetTickCount64());
    FILETIME  ft;
    s_origGetSystemTimeAsFileTime(&ft);

    double oldSpeed = s_speed.load(std::memory_order_relaxed);
    long long oldAnchorTick = s_anchorTickMs.load(std::memory_order_relaxed);
    long long oldAnchorAbs  = s_anchorAbsMs.load(std::memory_order_relaxed);
    long long oldOffset     = s_offsetMs.load(std::memory_order_relaxed);

    long long oldDelta = static_cast<long long>(
        (realTick - oldAnchorTick) * oldSpeed + oldOffset);

    s_anchorTickMs.store(realTick, std::memory_order_relaxed);
    s_anchorAbsMs.store(FileTimeToMs(ft), std::memory_order_relaxed);
    s_offsetMs.store(oldDelta, std::memory_order_relaxed);
    s_speed.store(speed, std::memory_order_relaxed);
}

void TimeHooks::Reset()
{
    if (!s_enabled.load(std::memory_order_relaxed)) return;

    // 重新锚定到"真实时间"，平滑归零
    long long realTick = static_cast<long long>(s_origGetTickCount64());
    FILETIME  ft;
    s_origGetSystemTimeAsFileTime(&ft);

    s_anchorTickMs.store(realTick, std::memory_order_relaxed);
    s_anchorAbsMs.store(FileTimeToMs(ft), std::memory_order_relaxed);
    s_offsetMs.store(0, std::memory_order_relaxed);
    s_speed.store(1.0, std::memory_order_relaxed);
}

long long TimeHooks::GetOffsetMs() { return s_offsetMs.load(); }
double    TimeHooks::GetSpeed()    { return s_speed.load(); }

// ---------------------------------------------------------------------------
// 虚拟时间计算
// ---------------------------------------------------------------------------
long long TimeHooks::ComputeDeltaMs()
{
    if (!s_enabled.load(std::memory_order_acquire)) return 0;

    double    speed   = s_speed.load(std::memory_order_relaxed);
    long long anchorT = s_anchorTickMs.load(std::memory_order_relaxed);
    long long offset  = s_offsetMs.load(std::memory_order_relaxed);

    long long realTick = static_cast<long long>(s_origGetTickCount64());
    return static_cast<long long>((realTick - anchorT) * speed) + offset;
}

ULARGE_INTEGER TimeHooks::GetVirtualFileTime()
{
    FILETIME ft;
    s_origGetSystemTimeAsFileTime(&ft);
    long long delta = ComputeDeltaMs();
    AddDeltaToFileTime(ft, delta);

    ULARGE_INTEGER v;
    v.LowPart  = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return v;
}

long long TimeHooks::GetVirtualNowMs()
{
    FILETIME ft;
    s_origGetSystemTimeAsFileTime(&ft);
    return FileTimeToMs(ft) + ComputeDeltaMs();
}

long long TimeHooks::GetRealNowMs()
{
    FILETIME ft;
    s_origGetSystemTimeAsFileTime(&ft);
    return FileTimeToMs(ft);
}

// ---------------------------------------------------------------------------
// Hook 替换函数
// ---------------------------------------------------------------------------
void WINAPI TimeHooks::HookedGetSystemTimeAsFileTime(LPFILETIME lpSystemTime)
{
    if (!lpSystemTime) return;

    // 未启用 -> 真实时间
    if (!s_enabled.load(std::memory_order_acquire)) {
        s_origGetSystemTimeAsFileTime(lpSystemTime);
        return;
    }

    s_origGetSystemTimeAsFileTime(lpSystemTime);
    AddDeltaToFileTime(*lpSystemTime, ComputeDeltaMs());
}

void WINAPI TimeHooks::HookedGetSystemTime(LPSYSTEMTIME lpSystemTime)
{
    if (!lpSystemTime) return;

    SYSTEMTIME st;
    if (!s_enabled.load(std::memory_order_acquire)) {
        s_origGetSystemTime(&st);
        *lpSystemTime = st;
        return;
    }

    FILETIME ft;
    s_origGetSystemTimeAsFileTime(&ft);
    AddDeltaToFileTime(ft, ComputeDeltaMs());
    FileTimeToSystemTime(&ft, &st);
    *lpSystemTime = st;
}

void WINAPI TimeHooks::HookedGetLocalTime(LPSYSTEMTIME lpSystemTime)
{
    if (!lpSystemTime) return;

    SYSTEMTIME st;
    if (!s_enabled.load(std::memory_order_acquire)) {
        s_origGetLocalTime(&st);
        *lpSystemTime = st;
        return;
    }

    FILETIME ft;
    s_origGetSystemTimeAsFileTime(&ft);
    AddDeltaToFileTime(ft, ComputeDeltaMs());
    FileTimeToSystemTime(&ft, &st);
    *lpSystemTime = st;
}

ULONGLONG WINAPI TimeHooks::HookedGetTickCount64()
{
    ULONGLONG real = s_origGetTickCount64();
    if (!s_enabled.load(std::memory_order_acquire)) return real;

    long long v = static_cast<long long>(real) + ComputeDeltaMs();
    return (v < 0) ? 0ULL : static_cast<ULONGLONG>(v);
}

BOOL WINAPI TimeHooks::HookedGetFileTime(
    HANDLE hFile, LPFILETIME lpCreationTime,
    LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
{
    BOOL ok = s_origGetFileTime(hFile, lpCreationTime,
                                lpLastAccessTime, lpLastWriteTime);
    if (ok && s_enabled.load(std::memory_order_acquire)) {
        long long delta = ComputeDeltaMs();
        if (lpCreationTime) AddDeltaToFileTime(*lpCreationTime, delta);
        if (lpLastAccessTime) AddDeltaToFileTime(*lpLastAccessTime, delta);
        if (lpLastWriteTime)  AddDeltaToFileTime(*lpLastWriteTime, delta);
    }
    return ok;
}

} // namespace usemytime
