# UseMyTime

**进程级 Windows 系统时间劫持工具** —— 手动指定游戏进程，注入并修改该进程获取到的系统时间（快进 / 时间跳跃 / 变速），**不影响全局 Windows 系统时间**。

> ⚠️ 仅供学习与研究。对受反作弊保护的游戏使用本工具可能导致封号；请勿用于任何作弊或非法用途。

---

## 功能特性

| 能力 | 说明 |
|---|---|
| 进程选择 | 枚举当前会话可注入进程（过滤系统关键进程），显示位数 |
| 一键注入 | 远程线程 DLL 注入（位数匹配检查 + 管理员提权 manifest） |
| 时间劫持 | Hook `GetSystemTime` / `GetLocalTime` / `GetSystemTimeAsFileTime` / `GetTickCount64` / `GetFileTime` |
| 虚拟时间 | 时间偏移（时:分:秒，可正可负）+ 流速（0 ~ 100x，无跳变切换） |
| 实时状态 | 500ms 轮询显示真实/虚拟时间、偏移、流速 |
| 安全恢复 | 三重回退：① UI「恢复真实时间」② 宿主退出时 IPC shutdown ③ 目标进程退出时 `DllMain(DLL_PROCESS_DETACH)` 自动 Unhook |
| 稳定 IPC | 命名管道 `\\.\pipe\UseMyTime_<pid>`，行分隔 JSON，全链路异常捕获，绝不影响目标游戏 |
| UI 不卡死 | 全部注入 / IPC 操作 `async/await` + 后台线程，状态轮询用 `System.Threading.Timer` |

---

## 目录结构

```
usemytime/
├── UseMyTime.sln                    # C++ 解决方案（根目录，MSBuild 入口）
├── .github/workflows/build.yml      # CI/CD（windows-latest, MSBuild + dotnet, Artifact）
├── .gitignore
├── native/                          # C/C++ hook DLL
│   └── UseMyTimeHook/
│       ├── UseMyTimeHook.vcxproj(.filters)
│       └── source/
│           ├── DllMain.cpp          # 入口：ATTACH 起 worker；DETACH 自动 Unhook
│           ├── TimeHooks.h/.cpp     # 虚拟时间模型 + 5 个 API hook（MinHook）
│           ├── IpcServer.h/.cpp     # 命名管道服务端（独立线程，异常全捕获）
│           ├── MiniJson.h/.cpp      # 极简 JSON 工具（无第三方依赖）
│           └── MinHook/             # MinHook 源码（TsudaKageyu, MIT 风格许可）
├── app/UseMyTime.App/               # C# WinForms 宿主 (net8.0-windows)
│   ├── UseMyTime.App.csproj
│   ├── app.manifest                 # requireAdministrator
│   ├── Program.cs
│   ├── MainForm.cs                  # UI（全 async）
│   └── Services/
│       ├── ProcessInjection.cs      # 远程线程注入（P/Invoke）
│       ├── IpcClient.cs             # 命名管道客户端（async，永不抛异常）
│       └── ProcessListService.cs    # 进程枚举
└── scripts/
    ├── init_git.ps1                 # Git 初始化（Windows PowerShell）
    └── init_git.sh                  # Git 初始化（macOS / Linux / Git-Bash）
```

---

## 工作原理

### 1. 虚拟时间模型（无漂移）

```
deltaMs  = (realTickNow − anchorTick) × speed + offsetMs
virtualNow = realNow + deltaMs
```

- `realTick` 用**真实**（未 hook 的）`GetTickCount64`（单调时钟）
- `realNow` 用**真实**（未 hook 的）`GetSystemTimeAsFileTime`（绝对时间）
- 修改 offset / speed 时**重新锚定**，保证时间连续、不跳变
- 所有 hook 内部先调原函数再叠加 delta，未启用时直接透传真实时间

### 2. IPC 协议（命名管道）

管道名：`\\.\pipe\UseMyTime_<目标pid>`，每行一条 JSON。

请求：

```json
{"cmd":"set_offset","ms":3600000}
{"cmd":"set_speed","speed":2.0}
{"cmd":"reset"}
{"cmd":"status"}
{"cmd":"shutdown"}
```

响应：

```json
{"ok":true,"offset_ms":3600000,"speed":2.0,"enabled":1,
 "real_time":"2026-08-27T20:00:00","virtual_time":"2026-08-27T21:00:00","delta_ms":3600000}
```

### 3. 安全回退链

```
UI「恢复真实时间」 ──IPC reset──►  TimeHooks::Reset()
宿主窗口关闭      ──IPC shutdown─►  TimeHooks::Shutdown() + Unhook
目标进程退出      ─DllMain DETACH─►  IpcServer::Stop() + TimeHooks::Shutdown()
```

任何一环失败，下一环兜底；DLL 内所有可能抛异常的路径均有 `try/catch`，
异常绝不出线程 / DllMain，**不会导致目标游戏崩溃**。

---

## 构建

### 本地（Windows + VS2022）

```powershell
# 1. C++ hook DLL（产物: bin/x64/Release/UseMyTimeHook.dll）
msbuild UseMyTime.sln /t:Build /p:Configuration=Release /p:Platform=x64

# 2. C# 宿主（自动把 DLL 拷到输出目录）
dotnet build app/UseMyTime.App -c Release -a x64

# 3. 运行（需要管理员权限）
.\app\UseMyTime.App\bin\x64\Release\net8.0-windows\UseMyTime.exe
```

### CI（GitHub Actions）

push 到 `main` 自动触发 `.github/workflows/build.yml`：
`windows-latest` → MSBuild 编 DLL → `dotnet build` 编 exe → 打包 Artifact `usemytime-windows-x64`。

---

## 使用流程

1. 以**管理员**身份启动 `UseMyTime.exe`
2. 选择游戏进程 → **一键注入**
3. 设置时间偏移（如 `+01:00:00`）与流速（如 `2.00x`）→ **应用虚拟时间**
4. 观察状态区：真实时间 vs 虚拟时间
5. 结束后点 **恢复真实时间**（或直接关游戏 / 关本工具，均会自动恢复）

---

## 已知限制

- 需要管理员权限（注入 elevate 进程）；64 位宿主只能注入 64 位目标
- 反作弊（EAC / BattlEye / Vanguard 等）会检测 DLL 注入与 hook，**会被拦截或封号**
- 部分游戏使用 `QueryPerformanceCounter` / RDTSC 计时，本工具未 hook（可按需扩展）
- 注入受保护的 32 位游戏需使用 32 位版本的宿主（工程已含 Win32 配置）

## 许可

- 本项目代码：MIT
- [MinHook](https://github.com/TsudaKageyu/MinHook)：Copyright (C) 2009-2017 Tsuda Kageyu（见 `native/UseMyTimeHook/source/MinHook/LICENSE.txt`）
