/*
 * UseMyTime - ProcessInjection
 *
 * 经典远程线程 DLL 注入：
 *   OpenProcess -> VirtualAllocEx -> WriteProcessMemory(dllPath)
 *   -> CreateRemoteThread(LoadLibraryA) -> 等待加载 -> 关闭句柄
 *
 * 注意：
 *   - 需要以管理员权限运行（注入受保护/ elevate 的进程）
 *   - 64 位宿主只能注入 64 位目标（位数匹配检查）
 */
using System.Runtime.InteropServices;

namespace UseMyTime.App.Services;

public sealed class ProcessInjection
{
    // ------------------------------------------------------------------
    // Win32 P/Invoke
    // ------------------------------------------------------------------
    private const uint PROCESS_CREATE_THREAD = 0x0002;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x0800;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_DUP_HANDLE = 0x0040;

    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint PAGE_READWRITE = 0x04;
    private const uint INFINITE = 0xFFFFFFFF;

    private static class Native
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern IntPtr OpenProcess(uint dwDesiredAccess,
            bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress,
            uint dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress,
            uint dwSize, uint dwFreeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
            byte[]? lpBuffer, uint nSize, out uint lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress,
            byte[]? lpBuffer, uint nSize, out uint lpNumberOfBytesRead);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes,
            uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter,
            uint dwCreationFlags, out uint lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint GetExitCodeThread(IntPtr hThread, out uint lpExitCode);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr GetModuleHandleA(string? lpModuleName);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool IsWow64Process(IntPtr hProcess, out bool wow64);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern uint GetLastError();
    }

    // LoadLibraryA 委托（远端线程入口）
    private delegate IntPtr LoadLibraryADelegate(string lpFileName);
    private const string LOAD_LIBRARY = "LoadLibraryA";

    // ------------------------------------------------------------------
    // 注入
    // ------------------------------------------------------------------

    /// <summary>
    /// 将 DLL 注入目标进程。成功返回 true。
    /// 整个方法在调用方（UI 线程外）执行 —— 内部全部是阻塞 Win32 调用。
    /// </summary>
    public static bool Inject(int pid, string dllPath, out string error)
    {
        error = string.Empty;

        IntPtr hProcess = IntPtr.Zero;
        IntPtr remoteMem = IntPtr.Zero;
        IntPtr hThread = IntPtr.Zero;

        try
        {
            // 1. 打开进程
            hProcess = Native.OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
                PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_DUP_HANDLE,
                false, pid);
            if (hProcess == IntPtr.Zero)
            {
                error = $"无法打开进程 (pid={pid})，错误码 {Marshal.GetLastWin32Error()}。" +
                        "可能需要管理员权限，或进程位数不匹配。";
                return false;
            }

            // 2. 位数匹配检查（64 位宿主 vs 32 位目标 -> 注入会失败）
            bool targetIsWow64;
            if (Native.IsWow64Process(hProcess, out targetIsWow64))
            {
                bool hostIs64 = Environment.Is64BitOperatingSystem &&
                                Environment.Is64BitProcess;
                if (hostIs64 && !targetIsWow64)
                {
                    error = $"目标进程 (pid={pid}) 是 32 位，当前宿主是 64 位，无法注入。";
                    return false;
                }
                if (!hostIs64 && targetIsWow64)
                {
                    error = $"目标进程 (pid={pid}) 是 64 位，当前宿主是 32 位，无法注入。";
                    return false;
                }
            }

            // 3. 分配远端内存并写入 DLL 路径（宽字符，含结尾 \0）
            byte[] pathBytes = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
            remoteMem = Native.VirtualAllocEx(hProcess, IntPtr.Zero,
                (uint)pathBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remoteMem == IntPtr.Zero)
            {
                error = $"VirtualAllocEx 失败，错误码 {Marshal.GetLastWin32Error()}";
                return false;
            }

            if (!Native.WriteProcessMemory(hProcess, remoteMem, pathBytes,
                    (uint)pathBytes.Length, out _))
            {
                error = $"WriteProcessMemory 失败，错误码 {Marshal.GetLastWin32Error()}";
                return false;
            }

            // 4. 取 kernel32!LoadLibraryA 地址（目标进程中）
            IntPtr kernel32 = Native.GetModuleHandleA("kernel32.dll");
            IntPtr loadLib = Native.GetProcAddress(kernel32, LOAD_LIBRARY);
            if (loadLib == IntPtr.Zero)
            {
                error = "无法解析 LoadLibraryA 地址";
                return false;
            }

            // 5. 创建远端线程执行 LoadLibraryA(dllPath)
            hThread = Native.CreateRemoteThread(hProcess, IntPtr.Zero, 0,
                loadLib, remoteMem, 0, out _);
            if (hThread == IntPtr.Zero)
            {
                error = $"CreateRemoteThread 失败，错误码 {Marshal.GetLastWin32Error()}。" +
                        "部分反作弊/受保护进程会拒绝创建远程线程。";
                return false;
            }

            // 6. 等待 DLL 加载完成（DllMain 执行完毕）
            Native.WaitForSingleObject(hThread, 15000);
            Native.GetExitCodeThread(hThread, out uint exitCode);

            return true; // DllMain 已执行，hook 由 DLL 内部 worker 线程安装
        }
        finally
        {
            // 7. 清理（远端内存保留：LoadLibrary 已复制路径，释放无副作用）
            if (hThread != IntPtr.Zero) Native.CloseHandle(hThread);
            if (remoteMem != IntPtr.Zero)
            {
                // 尽力释放（部分系统上 LoadLibrary 后内存已被释放会失败，忽略）
                Native.VirtualFreeEx(hProcess, remoteMem, 0, 0x3000);
            }
            if (hProcess != IntPtr.Zero) Native.CloseHandle(hProcess);
        }
    }
}
