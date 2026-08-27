# Role
你是一个资深的 Windows C#/C++ 逆向工程与开发专家。请帮我从零开发一个名为 "UseMyTime" 的 Windows 系统时间劫持工具，并配置完整的工程与 CI/CD 环境。

# Project Context
工具的作用是：通过手动指定游戏进程，注入并修改该进程获取到的系统时间（实现快进/时间跳跃），而不影响全局 Windows 系统时间。

# Core Requirements
请按顺序执行并输出以下任务的代码与操作脚本：

1. **底层 Hook 与 DLL 注入 (C/C++)**
   * 编写一个 DLL，使用 MinHook 或类似轻量级库拦截与时间相关的 Windows API (如 `GetSystemTime`, `GetLocalTime`, `GetSystemTimeAsFileTime`, `GetTickCount64`)。
   * 实现异常处理：在 DLL 的 `DllMain` 捕获 `DLL_PROCESS_DETACH`，确保当注入端意外断开时，自动执行 Unhook，恢复真实系统时间。

2. **前端控制台 (C# WinForms / WPF)**
   * 编写一个 C# 窗体程序，包含：进程列表刷新、目标进程选择、虚拟时间设置输入框、一键注入/修改按钮。
   * **强制要求 (修复已知 Bug)**：严禁在主线程执行注入或通信操作。必须使用 `async/await` 或 `BackgroundWorker` 防止 UI 卡死。

3. **稳定进程间通信 (IPC)**
   * 在 C# 宿主和 C++ DLL 之间建立高可用通信。建议使用 **Named Pipes (命名管道)** 替代低效的跨线程消息。即使接收新时间失败，也必须捕获异常，绝不能导致目标游戏崩溃。

4. **DevOps 自动化 (Git & GitHub Actions)**
   * 提供在工作目录初始化 Git 仓库的 Shell/PowerShell 脚本。
   * 编写一个 `.github/workflows/build.yml` 文件。触发条件为 `push` 到 `main` 分支，使用 `windows-latest` 运行环境，包含 MSBuild (编译 C++ DLL) 和 `dotnet build` (编译 C# 窗体) 的完整 Action 流程，并将生成的 `.exe` 和 `.dll` 打包为 Artifact 输出。

# Output Format
请分步骤给出清晰的代码结构、核心类的完整实现，以及必要的配置文件。

