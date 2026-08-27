/*
 * UseMyTime - DllMain（DLL 入口）
 *
 * 设计要点：
 *   1. DllMain 中只做"轻量"工作：
 *        - ATTACH  : 启动一个独立 worker 线程执行 Initialize
 *                    （MinHook 的 trampoline 分配、线程创建等操作
 *                      在 loader lock 下做是不安全的，必须放到独立线程）
 *        - DETACH  : 立即执行 Shutdown（Unhook + 停止 IPC 线程）
 *                    —— 这是需求中的强制要求：注入端意外断开时，
 *                       自动恢复真实系统时间，绝不残留 hook。
 *   2. 导出 InitUseMyTime() / ShutdownUseMyTime() 供注入器显式调用，
 *      与 DllMain 的自动行为互为兜底（幂等）。
 *   3. 所有可能抛异常的路径都有 try/catch 保护 —— 绝不让异常
 *      逃逸出 DllMain（会导致宿主进程直接崩溃）。
 */
#include <windows.h>

#include "TimeHooks.h"
#include "IpcServer.h"

using namespace usemytime;

// ---------------------------------------------------------------------------
// worker 线程：在 DllMain 返回后执行真正的初始化
// ---------------------------------------------------------------------------
static HANDLE g_workerThread = nullptr;
static volatile LONG g_initialized = 0;

static DWORD WINAPI WorkerThread(LPVOID)
{
    // 异常保护：worker 线程逃逸的异常同样会崩溃宿主进程
    try {
        // 1. 安装时间 hook
        if (!TimeHooks::Initialize()) {
            OutputDebugStringA("UseMyTime: TimeHooks::Initialize failed\n");
            return 1;
        }
        // 2. 启动 IPC 服务（命名管道）
        IpcServer::Start();
        InterlockedExchange(&g_initialized, 1);
        OutputDebugStringA("UseMyTime: initialized\n");
    }
    catch (...) {
        OutputDebugStringA("UseMyTime: worker exception swallowed\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        // 轻量：仅创建（挂起状态）worker 线程
        g_workerThread = CreateThread(nullptr, 0, WorkerThread,
                                      nullptr, CREATE_SUSPENDED, nullptr);
        if (g_workerThread) ResumeThread(g_workerThread);
        break;
    }

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;

    case DLL_PROCESS_DETACH: {
        // ==================================================================
        // 强制要求：进程卸载时自动 Unhook，恢复真实系统时间。
        // 场景：注入器（C# 宿主）意外断开 / 被杀死 / 游戏退出。
        // ==================================================================
        try {
            // 1. 先停 IPC 服务（其线程退出前也会兜底 Shutdown）
            IpcServer::Stop();

            // 2. 等待 worker 完成初始化（最多 2 秒），避免
            //    "worker 正在装 hook，DllMain 同时在卸" 的竞争
            if (g_workerThread) {
                WaitForSingleObject(g_workerThread, 2000);
            }

            // 3. 卸载全部 hook -> 恢复真实系统时间
            TimeHooks::Shutdown();
        }
        catch (...) {
            // 最后防线：绝不让异常逃逸出 DllMain
        }

        if (g_workerThread) {
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
        break;
    }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 导出函数（供注入器显式控制）
// ---------------------------------------------------------------------------
extern "C" {

// 显式初始化（幂等）。返回 1 成功 / 0 失败。
BOOL WINAPI InitUseMyTime()
{
    try {
        bool ok = TimeHooks::Initialize();
        IpcServer::Start();
        return ok ? TRUE : FALSE;
    }
    catch (...) { return FALSE; }
}

// 显式关闭：卸载 hook 恢复真实时间（幂等）。
BOOL WINAPI ShutdownUseMyTime()
{
    try {
        IpcServer::Stop();
        TimeHooks::Shutdown();
        return TRUE;
    }
    catch (...) { return FALSE; }
}

// 查询是否已初始化
BOOL WINAPI IsUseMyTimeActive()
{
    return TimeHooks::IsEnabled() ? TRUE : FALSE;
}

} // extern "C"
