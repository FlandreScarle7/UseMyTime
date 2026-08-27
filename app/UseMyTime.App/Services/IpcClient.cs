/*
 * UseMyTime - IpcClient
 *
 * 命名管道客户端（C# 宿主侧）：
 *   - 管道名与 DLL 侧一致：\\.\pipe\UseMyTime_<pid>
 *   - 协议：每行一条 JSON（与 MiniJson 对应）
 *
 * 高可用设计（满足"即使接收新时间失败也不能导致目标游戏崩溃"）：
 *   1. 所有方法均为 async，内部完整 try/catch，绝不向调用方抛异常
 *   2. 每次调用独立连接（短连接），避免长连接状态腐化
 *   3. 连接/读写均有超时，失败时返回 (false, error)
 *   4. 连接前先探测目标进程是否存活
 */
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace UseMyTime.App.Services;

public sealed class IpcClient
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ReadTimeout = TimeSpan.FromSeconds(5);

    /// <summary>目标进程是否仍存活</summary>
    public static bool IsProcessAlive(int pid)
    {
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(pid);
            return !p.HasExited;
        }
        catch { return false; }
    }

    /// <summary>
    /// 向目标进程发送一条 JSON 命令并读取一行 JSON 响应。
    /// 永不抛异常：失败返回 (false, errorMessage)。
    /// </summary>
    public static async Task<(bool ok, string response, string error)>
        SendAsync(int pid, string jsonCommand, CancellationToken ct = default)
    {
        try
        {
            if (!IsProcessAlive(pid))
                return (false, "", $"目标进程 (pid={pid}) 已退出。");

            string pipeName = $@"\\.\pipe\UseMyTime_{pid}";

            using var pipe = new NamedPipeClientStream(
                ".", pipeName, PipeDirection.InOut, 8, PipeOptions.Asynchronous);

            // 带超时的连接
            var connectTask = pipe.ConnectAsync(ct);
            var timeoutTask = Task.Delay(ConnectTimeout, ct);
            var finished = await Task.WhenAny(connectTask, timeoutTask);

            if (finished == timeoutTask)
                return (false, "", $"连接管道超时（目标进程可能未注入或 IPC 服务未启动）。");

            try { await connectTask; }
            catch (Exception ex)
            {
                return (false, "", $"连接管道失败：{ex.Message}");
            }

            if (!pipe.IsConnected)
                return (false, "", "管道未连接。");

            // 写命令（行分隔）
            byte[] payload = Encoding.UTF8.GetBytes(jsonCommand + "\n");
            await pipe.WriteAsync(payload, ct);
            await pipe.FlushAsync(ct);

            // 读一行响应（带总超时，循环读取直到换行符）
            var sb = new StringBuilder();
            var buffer = new byte[8192];
            using var readTimeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            readTimeout.CancelAfter(ReadTimeout);

            try
            {
                while (true)
                {
                    int bytesRead = await pipe.ReadAsync(buffer, 0, buffer.Length, readTimeout.Token);
                    if (bytesRead <= 0)
                        return (false, "", "管道已关闭（目标进程可能已退出）。");
                    sb.Append(Encoding.UTF8.GetString(buffer, 0, bytesRead));
                    if (sb.ToString().Contains('\n'))
                        break;
                    if (sb.Length > 65536) // 防御：异常超长
                        break;
                }
            }
            catch (OperationCanceledException)
            {
                return (false, "", "读取响应超时。");
            }

            int nl = sb.ToString().IndexOf('\n');
            string line = (nl >= 0 ? sb.ToString(0, nl) : sb.ToString()).Trim();
            if (line.Length == 0)
                return (false, "", "响应为空。");
            return (true, line, "");
        }
        catch (OperationCanceledException)
        {
            return (false, "", "操作已取消。");
        }
        catch (Exception ex)
        {
            // 最后防线：任何未预期异常都不向 UI 层传播
            return (false, "", $"IPC 异常：{ex.Message}");
        }
    }

    // ------------------------------------------------------------------
    // 高层命令封装（全部 async，永不抛异常）
    // ------------------------------------------------------------------

    /// <summary>设置时间偏移（毫秒）。正数=快进到未来，负数=回退到过去。</summary>
    public static async Task<(bool ok, TimeStatus? status, string error)>
        SetOffsetAsync(int pid, long offsetMs, CancellationToken ct = default)
    {
        var (ok, resp, err) = await SendAsync(
            pid, $"{{\"cmd\":\"set_offset\",\"ms\":{offsetMs}}}", ct);
        return ParseStatus(ok, resp, err);
    }

    /// <summary>设置时间流速（1.0=正常，2.0=两倍速）。</summary>
    public static async Task<(bool ok, TimeStatus? status, string error)>
        SetSpeedAsync(int pid, double speed, CancellationToken ct = default)
    {
        var (ok, resp, err) = await SendAsync(
            pid, $"{{\"cmd\":\"set_speed\",\"speed\":{speed.ToString(System.Globalization.CultureInfo.InvariantCulture)}}}", ct);
        return ParseStatus(ok, resp, err);
    }

    /// <summary>恢复真实时间。</summary>
    public static async Task<(bool ok, TimeStatus? status, string error)>
        ResetAsync(int pid, CancellationToken ct = default)
    {
        var (ok, resp, err) = await SendAsync(pid, "{\"cmd\":\"reset\"}", ct);
        return ParseStatus(ok, resp, err);
    }

    /// <summary>查询当前状态。</summary>
    public static async Task<(bool ok, TimeStatus? status, string error)>
        GetStatusAsync(int pid, CancellationToken ct = default)
    {
        var (ok, resp, err) = await SendAsync(pid, "{\"cmd\":\"status\"}", ct);
        return ParseStatus(ok, resp, err);
    }

    /// <summary>通知目标卸载 hook 并恢复真实时间（优雅退出）。</summary>
    public static async Task<(bool ok, string error)>
        ShutdownAsync(int pid, CancellationToken ct = default)
    {
        var (ok, resp, err) = await SendAsync(pid, "{\"cmd\":\"shutdown\"}", ct);
        if (!ok) return (false, err);
        try
        {
            using var doc = JsonDocument.Parse(resp);
            return (doc.RootElement.GetProperty("ok").GetBoolean(), "");
        }
        catch { return (true, ""); } // 收不到完整响应也视为已发送
    }

    // ------------------------------------------------------------------
    // 响应解析
    // ------------------------------------------------------------------
    private static (bool ok, TimeStatus? status, string error)
        ParseStatus(bool transportOk, string response, string error)
    {
        if (!transportOk) return (false, null, error);
        try
        {
            using var doc = JsonDocument.Parse(response);
            var root = doc.RootElement;
            var status = new TimeStatus
            {
                Ok = root.TryGetProperty("ok", out var o) && o.GetBoolean(),
                OffsetMs = root.TryGetProperty("offset_ms", out var om) ? om.GetInt64() : 0,
                Speed = root.TryGetProperty("speed", out var s) ? s.GetDouble() : 1.0,
                Enabled = root.TryGetProperty("enabled", out var e) && e.GetInt32() == 1,
                RealTime = root.TryGetProperty("real_time", out var rt) ? rt.GetString() ?? "" : "",
                VirtualTime = root.TryGetProperty("virtual_time", out var vt) ? vt.GetString() ?? "" : "",
                DeltaMs = root.TryGetProperty("delta_ms", out var d) ? d.GetInt64() : 0,
            };
            if (!status.Ok)
            {
                string errMsg = root.TryGetProperty("error", out var ep) ? ep.GetString() ?? "unknown" : "unknown";
                return (false, status, errMsg);
            }
            return (true, status, "");
        }
        catch (Exception ex)
        {
            return (false, null, $"响应解析失败：{ex.Message}");
        }
    }
}

/// <summary>目标进程内的时间状态快照</summary>
public sealed class TimeStatus
{
    public bool Ok { get; init; }
    public long OffsetMs { get; init; }
    public double Speed { get; init; }
    public bool Enabled { get; init; }
    public string RealTime { get; init; } = "";
    public string VirtualTime { get; init; } = "";
    public long DeltaMs { get; init; }
}
