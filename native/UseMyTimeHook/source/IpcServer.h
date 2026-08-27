/*
 * UseMyTime - IpcServer
 *
 * 命名管道 IPC 服务端（运行在目标进程内的独立线程）：
 *   - 管道名：\\.\pipe\UseMyTime_<pid>
 *   - 协议：  每行一条 JSON 命令，每行一条 JSON 响应（LF 分隔）
 *
 * 命令（request）:
 *   {"cmd":"set_offset","ms":3600000}          设置时间偏移
 *   {"cmd":"set_speed","speed":2.0}            设置流速
 *   {"cmd":"reset"}                            恢复真实时间
 *   {"cmd":"status"}                           查询当前状态
 *   {"cmd":"shutdown"}                         卸载 hook，恢复真实时间
 *
 * 响应（response）:
 *   {"ok":true,"offset_ms":3600000,"speed":2.0,
 *    "real_time":"2026-08-27T20:00:00","virtual_time":"2026-08-27T21:00:00"}
 *   {"ok":false,"error":"unknown command"}
 *
 * 高可用设计：
 *   - 整个服务端线程用 try/catch 包裹，任何异常只记录日志，绝不影响宿主进程
 *   - 客户端断开 / 管道关闭时自动回到等待状态
 *   - 收到 shutdown 或宿主退出信号时自行退出
 */
#pragma once

#include <windows.h>
#include <atomic>
#include <string>

namespace usemytime {

class IpcServer {
public:
    // 启动 IPC 服务（创建独立线程）。重复调用幂等。
    static bool Start();

    // 停止 IPC 服务并退出线程。
    static void Stop();

    // 当前管道名
    static std::string PipeName();

    // 服务是否正在运行
    static bool IsRunning();

private:
    static DWORD WINAPI ServerThread(LPVOID);

    static std::atomic<bool>       s_running;
    static std::atomic<bool>       s_started;
    static HANDLE                  s_thread;
    static HANDLE                  s_stopEvent;
    static HANDLE                  s_pipe;
};

} // namespace usemytime
