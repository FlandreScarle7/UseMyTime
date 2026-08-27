/*
 * UseMyTime - ProcessListService
 *
 * 枚举可注入的目标进程（过滤系统关键进程，只保留用户可见/可注入候选）。
 */
using System.Diagnostics;

namespace UseMyTime.App.Services;

public sealed record ProcessInfo(int Id, string Name, string Path, bool Is64Bit);

public static class ProcessListService
{
    // 关键系统进程：注入可能导致系统不稳定，默认隐藏
    private static readonly HashSet<string> BlockedNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "System", "csrss.exe", "wininit.exe", "winlogon.exe", "lsass.exe",
        "services.exe", "smss.exe", "dwm.exe", "svchost.exe", "fontdrvhost.exe",
        "SearchIndexer.exe", "Memory Compression", "Registry",
    };

    /// <summary>
    /// 枚举当前所有用户可注入的进程（按名称排序）。
    /// 在后台线程调用（内部是同步枚举）。
    /// </summary>
    public static List<ProcessInfo> GetInjectableProcesses()
    {
        var result = new List<ProcessInfo>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (p.SessionId != Environment.CurrentSession)
                {
                    p.Dispose();
                    continue; // 只列当前会话
                }

                string name = p.ProcessName + ".exe";
                if (BlockedNames.Contains(name) || BlockedNames.Contains(p.ProcessName))
                {
                    p.Dispose();
                    continue;
                }

                // 去重（同名进程只保留一条，UI 中显示 pid 区分）
                string key = p.ProcessName;
                if (!seen.Add(key))
                {
                    p.Dispose();
                    continue;
                }

                bool is64 = IsProcess64Bit(p.Id);
                string path = "";
                try { path = p.MainModule?.FileName ?? ""; }
                catch { /* 无权限读取路径，忽略 */ }

                result.Add(new ProcessInfo(p.Id, name, path, is64));
            }
            catch
            {
                // 进程可能已退出 / 无权限 —— 跳过
            }
            finally
            {
                p.Dispose();
            }
        }

        return result
            .Where(x => x.Id != Environment.ProcessId) // 排除自身
            .OrderBy(x => x.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    /// <summary>判断进程位数（失败返回宿主位数）</summary>
    private static bool IsProcess64Bit(int pid)
    {
        try
        {
            using var p = Process.GetProcessById(pid);
            if (NativeMethods.IsWow64Process(p.Handle, out bool wow64))
                return !wow64;
        }
        catch { }
        return Environment.Is64BitProcess;
    }

    private static class NativeMethods
    {
        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
        [return: System.Runtime.InteropServices.MarshalAs(
            System.Runtime.InteropServices.UnmanagedType.Bool)]
        public static extern bool IsWow64Process(
            System.Runtime.InteropServices.IntPtr hProcess,
            [System.Runtime.InteropServices.Out] out bool wow64Process);
    }
}
