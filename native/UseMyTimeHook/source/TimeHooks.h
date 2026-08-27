/*
 * UseMyTime - TimeHooks
 *
 * 时间劫持核心模块：
 *   - 通过 MinHook 拦截 kernel32 的时间相关 API
 *   - 维护"无漂移"虚拟时间模型（锚点 + 偏移 + 倍速）
 *   - 可随时 Reset / Shutdown 恢复真实系统时间
 *
 * 虚拟时间模型：
 *   deltaMs   = (realTickNow - anchorTick) * speed + offsetMs
 *   virtualNow = realNow + deltaMs
 *
 * 其中 realTick 使用真实（未 hook）的 GetTickCount64（单调递增），
 * realNow 使用真实（未 hook）的 GetSystemTimeAsFileTime（绝对时间）。
 */
#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>

#include "MinHook.h"

namespace usemytime {

// ---------------------------------------------------------------------------
// 被 hook 的原始函数指针
// ---------------------------------------------------------------------------
using FnGetSystemTimeAsFileTime =
    void(WINAPI*)(LPFILETIME lpSystemTime);
using FnGetSystemTime = void(WINAPI*)(LPSYSTEMTIME lpSystemTime);
using FnGetLocalTime = void(WINAPI*)(LPSYSTEMTIME lpSystemTime);
using FnGetTickCount64 = ULONGLONG(WINAPI*)();
using FnGetFileTime = BOOL(WINAPI*)(
    HANDLE hFile, LPFILETIME lpCreationTime,
    LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);

class TimeHooks {
public:
    // 安装全部 hook（必须在非 DllMain 线程调用，避免 loader lock）。
    // 成功返回 true；重复调用是幂等的。
    static bool Initialize();

    // 卸载全部 hook，恢复真实系统时间。幂等、异常安全。
    static void Shutdown();

    // 是否已安装 hook
    static bool IsEnabled();

    // 设置固定时间偏移（毫秒，正数=快进/未来，负数=回退/过去）。
    // 语义：虚拟时间立即跳到 (真实时间 + offsetMs)，之后按 speed 推进。
    static void SetOffsetMs(long long offsetMs);

    // 设置时间流速（1.0 = 正常，2.0 = 两倍速，0.5 = 半速）。连续、无跳变。
    static void SetSpeed(double speed);

    // 恢复真实时间（offset=0, speed=1）。
    static void Reset();

    // 当前状态查询（供 IPC 响应使用）
    static long long GetOffsetMs();
    static double GetSpeed();

    // 当前虚拟时间（FILETIME，1601 纪元，100ns 单位）
    static ULARGE_INTEGER GetVirtualFileTime();

    // 当前虚拟时间（毫秒，自 1601 纪元）
    static long long GetVirtualNowMs();

    // 当前真实时间（毫秒，自 1601 纪元）
    static long long GetRealNowMs();

private:
    // 被 hook 的替换函数
    static void WINAPI HookedGetSystemTimeAsFileTime(LPFILETIME lpSystemTime);
    static void WINAPI HookedGetSystemTime(LPSYSTEMTIME lpSystemTime);
    static void WINAPI HookedGetLocalTime(LPSYSTEMTIME lpSystemTime);
    static ULONGLONG WINAPI HookedGetTickCount64();
    static BOOL WINAPI HookedGetFileTime(
        HANDLE hFile, LPFILETIME lpCreationTime,
        LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);

    // 计算当前虚拟时间与真实时间的差值（毫秒）
    static long long ComputeDeltaMs();

    // 状态（锚点模型）
    static std::atomic<long long> s_anchorTickMs;   // 锚点时刻的真实 tick
    static std::atomic<long long> s_anchorAbsMs;    // 锚点时刻的真实绝对时间
    static std::atomic<long long> s_offsetMs;       // 固定偏移
    static std::atomic<double>    s_speed;          // 流速
    static std::atomic<bool>      s_enabled;        // hook 是否生效

    // 原始函数指针
    static FnGetSystemTimeAsFileTime s_origGetSystemTimeAsFileTime;
    static FnGetSystemTime           s_origGetSystemTime;
    static FnGetLocalTime            s_origGetLocalTime;
    static FnGetTickCount64          s_origGetTickCount64;
    static FnGetFileTime             s_origGetFileTime;

    // hook 替换函数指针（MH_CreateHook 的 pDetour 参数）
    static void* s_hookGetSystemTimeAsFileTime;
    static void* s_hookGetSystemTime;
    static void* s_hookGetLocalTime;
    static void* s_hookGetTickCount64;
    static void* s_hookGetFileTime;

    // 模块句柄（用于 MH_Unhook）
    static HMODULE s_module;
    static std::mutex s_initMutex;
};

} // namespace usemytime
