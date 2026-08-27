/*
 * UseMyTime - IpcServer 实现（命名管道服务端）
 */
#include "IpcServer.h"
#include "TimeHooks.h"
#include "MiniJson.h"

#include <cstdio>
#include <cstring>
#include <chrono>

namespace usemytime {

std::atomic<bool> IpcServer::s_running{ false };
std::atomic<bool> IpcServer::s_started{ false };
HANDLE IpcServer::s_thread    = nullptr;
HANDLE IpcServer::s_stopEvent = nullptr;
HANDLE IpcServer::s_pipe      = INVALID_HANDLE_VALUE;

std::string IpcServer::PipeName()
{
    return "\\\\.\\pipe\\UseMyTime_" +
           std::to_string(GetCurrentProcessId());
}

bool IpcServer::IsRunning() { return s_running.load(); }

bool IpcServer::Start()
{
    if (s_started.load(std::memory_order_acquire)) return true;

    s_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!s_stopEvent) return false;

    s_running.store(true, std::memory_order_relaxed);
    s_thread = CreateThread(nullptr, 0, ServerThread, nullptr,
                            CREATE_SUSPENDED, nullptr);
    if (!s_thread) {
        s_running.store(false);
        CloseHandle(s_stopEvent);
        s_stopEvent = nullptr;
        return false;
    }
    ResumeThread(s_thread);
    s_started.store(true, std::memory_order_release);
    return true;
}

void IpcServer::Stop()
{
    if (!s_started.load(std::memory_order_acquire)) return;

    s_running.store(false, std::memory_order_relaxed);
    if (s_stopEvent) SetEvent(s_stopEvent);

    // 注意：这里【不能】调用 CancelSynchronousIo(GetCurrentProcess())，
    // 那会取消宿主（游戏）进程的全部同步 IO，可能导致游戏崩溃。
    // 服务端线程通过 1s 轮询 s_running + 客户端断开（ReadFile 返回 0）自行退出。
    if (s_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    if (s_thread) {
        WaitForSingleObject(s_thread, 5000);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
    if (s_stopEvent) {
        CloseHandle(s_stopEvent);
        s_stopEvent = nullptr;
    }
    s_started.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// 命令处理
// ---------------------------------------------------------------------------
static std::string HandleCommand(const std::string& line)
{
    MiniJson req = MiniJson::Parse(line);
    std::string cmd = req.Get("cmd").AsString();

    std::string resp;
    if (cmd == "set_offset") {
        long long ms = static_cast<long long>(
            req.Get("ms").AsNumber(0.0));
        TimeHooks::SetOffsetMs(ms);
        resp = MiniJson::StatusResponse();
    }
    else if (cmd == "set_speed") {
        double speed = req.Get("speed").AsNumber(1.0);
        TimeHooks::SetSpeed(speed);
        resp = MiniJson::StatusResponse();
    }
    else if (cmd == "reset") {
        TimeHooks::Reset();
        resp = MiniJson::StatusResponse();
    }
    else if (cmd == "status") {
        resp = MiniJson::StatusResponse();
    }
    else if (cmd == "shutdown") {
        // 先卸载 hook 恢复真实时间，再返回响应
        TimeHooks::Shutdown();
        MiniJson r;
        r.SetBool("ok", true);
        r.SetString("msg", "shutdown");
        resp = r.Dump();
        s_running.store(false); // 触发服务端线程退出
    }
    else {
        MiniJson r;
        r.SetBool("ok", false);
        r.SetString("error", "unknown command");
        resp = r.Dump();
    }
    resp += "\n";
    return resp;
}

// ---------------------------------------------------------------------------
// 服务端主循环
// ---------------------------------------------------------------------------
DWORD WINAPI IpcServer::ServerThread(LPVOID)
{
    // 整个线程体包裹在异常保护中 —— 绝不让异常逃逸出线程
    // （逃逸会导致宿主进程崩溃，违反"绝不能导致目标游戏崩溃"的约束）
    try {
        const std::string pipeName = PipeName();
        const DWORD BUFSIZE = 8192;
        char buf[BUFSIZE];

        // 日志（仅调试用，Release 可关闭）
        OutputDebugStringA(("UseMyTime IPC server started: " + pipeName + "\n").c_str());

        while (s_running.load(std::memory_order_acquire)) {
            // 1. 创建管道实例
            s_pipe = CreateNamedPipeA(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | PIPE_READMODE_BYTE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, BUFSIZE, BUFSIZE, 3000 /*ms*/, nullptr);

            if (s_pipe == INVALID_HANDLE_VALUE) {
                // 创建失败：等待停止事件，避免忙等
                WaitForSingleObject(s_stopEvent, 100);
                continue;
            }

            // 2. 等待客户端连接（WaitNamedPipe 带超时轮询停止事件）
            bool clientConnected = false;
            while (s_running.load(std::memory_order_acquire)) {
                DWORD wr = WaitNamedPipeA(pipeName.c_str(), 1000);
                if (wr == ERROR_PIPE_CONNECTED) { clientConnected = true; break; }
                if (wr == ERROR_SEM_TIMEOUT)
                    continue; // 超时，继续轮询停止标志
                break; // 其他错误
            }

            if (!s_running.load(std::memory_order_acquire)) {
                CloseHandle(s_pipe);
                s_pipe = INVALID_HANDLE_VALUE;
                break;
            }

            // 3. 连接客户端（WaitNamedPipe 已确认有客户端等待）
            if (!clientConnected &&
                !ConnectNamedPipe(s_pipe, nullptr)) {
                CloseHandle(s_pipe);
                s_pipe = INVALID_HANDLE_VALUE;
                continue;
            }
            if (clientConnected) {
                // 客户端已在等待，ConnectNamedPipe 会返回 ERROR_PIPE_CONNECTED
                ConnectNamedPipe(s_pipe, nullptr);
            }

            OutputDebugStringA("UseMyTime IPC client connected\n");

            // 4. 读-处理-写 循环（按行）
            std::string pending;
            while (s_running.load(std::memory_order_acquire)) {
                DWORD bytes = 0;
                BOOL ok = ReadFile(s_pipe, buf, BUFSIZE - 1, &bytes, nullptr);
                if (!ok || bytes == 0) break; // 客户端断开

                buf[bytes] = '\0';
                pending.append(buf, bytes);

                // 逐行处理
                size_t pos;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, pos);
                    pending.erase(0, pos + 1);
                    // 去掉 \r
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty()) continue;

                    std::string resp = HandleCommand(line);
                    DWORD written = 0;
                    FlushFileBuffers(s_pipe);
                    if (!WriteFile(s_pipe, resp.c_str(),
                                   static_cast<DWORD>(resp.size()),
                                   &written, nullptr)) {
                        break; // 写失败 -> 客户端断开
                    }
                    if (!s_running.load(std::memory_order_acquire)) break;
                }
            }

            // 5. 断开客户端，回到等待状态
            DisconnectNamedPipe(s_pipe);
            CloseHandle(s_pipe);
            s_pipe = INVALID_HANDLE_VALUE;
        }
    }
    catch (...) {
        // 吞掉一切异常：保护宿主进程
        OutputDebugStringA("UseMyTime IPC server: exception swallowed\n");
    }

    // 线程退出前确保 hook 已卸载（兜底：宿主进程即将退出）
    try {
        TimeHooks::Shutdown();
    } catch (...) {}

    OutputDebugStringA("UseMyTime IPC server stopped\n");
    return 0;
}

} // namespace usemytime
