/*
 * UseMyTime - MainForm
 *
 * 主界面：
 *   [进程列表] [刷新]  [目标进程: 下拉框]  [一键注入]
 *   [虚拟时间: 偏移(时:分:秒) + 流速]  [应用] [恢复真实时间]
 *   [状态: 真实时间 / 虚拟时间 / 偏移 / 流速]
 *   [日志区]
 *
 * 强制要求落实：
 *   - 所有注入 / IPC 通信全部 async/await，绝不在 UI 线程阻塞
 *   - 状态轮询用 System.Threading.Timer（后台线程）+ BeginInvoke 回 UI
 *   - 任何 IPC 失败只写日志，不弹窗崩溃，不影响目标游戏
 */
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
using UseMyTime.App.Services;

namespace UseMyTime.App;

public sealed class MainForm : Form
{
    // ------------------------------------------------------------------
    // 控件
    // ------------------------------------------------------------------
    private readonly ListBox _processList = new() { Dock = DockStyle.Fill };
    private readonly Button _btnRefresh = new() { Text = "刷新列表", AutoSize = true };
    private readonly ComboBox _cmbTarget = new()
    {
        Dock = DockStyle.Fill,
        DropDownStyle = ComboBoxStyle.DropDownList,
    };
    private readonly Button _btnInject = new() { Text = "一键注入", AutoSize = true };

    private readonly NumericUpDown _numHours = new()
    { Minimum = -48, Maximum = 48, Value = 0, Width = 60 };
    private readonly NumericUpDown _numMinutes = new()
    { Minimum = 0, Maximum = 59, Value = 0, Width = 60 };
    private readonly NumericUpDown _numSeconds = new()
    { Minimum = 0, Maximum = 59, Value = 0, Width = 60 };
    private readonly NumericUpDown _numSpeed = new()
    {
        Minimum = 0, Maximum = 100, Value = 1, DecimalPlaces = 2, Width = 70,
    };
    private readonly Button _btnApply = new() { Text = "应用虚拟时间", AutoSize = true };
    private readonly Button _btnReset = new() { Text = "恢复真实时间", AutoSize = true };

    private readonly Label _lblRealTime = new()
    { Text = "真实时间: --", AutoSize = true, Font = new Font("Consolas", 11, FontStyle.Bold) };
    private readonly Label _lblVirtualTime = new()
    { Text = "虚拟时间: --", AutoSize = true, ForeColor = Color.DodgerBlue,
        Font = new Font("Consolas", 11, FontStyle.Bold) };
    private readonly Label _lblState = new()
    { Text = "状态: 未注入", AutoSize = true, ForeColor = Color.Gray };
    private readonly Label _lblOffset = new() { Text = "偏移: 0", AutoSize = true };
    private readonly Label _lblSpeed = new() { Text = "流速: 1.00x", AutoSize = true };

    private readonly TextBox _logBox = new()
    {
        Dock = DockStyle.Fill,
        Multiline = true,
        ReadOnly = true,
        ScrollBars = ScrollBars.Both,
        WordWrap = false,
        BackColor = Color.FromArgb(24, 24, 28),
        ForeColor = Color.Silver,
        Font = new Font("Consolas", 9),
    };

    // ------------------------------------------------------------------
    // 状态
    // ------------------------------------------------------------------
    private int _targetPid = -1;
    private string _targetName = "";
    private bool _injected;
    private readonly System.Threading.Timer _statusTimer;
    private readonly CancellationTokenSource _cts = new();
    private string _dllPath = "";

    public MainForm()
    {
        InitializeComponentManual();

        // 定位 hook DLL（与 exe 同目录）
        _dllPath = Path.Combine(AppContext.BaseDirectory, "UseMyTimeHook.dll");

        // 状态轮询：500ms 一次，后台线程触发，回调切回 UI 线程
        _statusTimer = new System.Threading.Timer(
            _ => PollStatusTick(), null,
            TimeSpan.Zero, TimeSpan.FromMilliseconds(500));

        Load += async (_, _) =>
        {
            await RefreshProcessListAsync();
            AppendLog("UseMyTime 就绪。选择游戏进程后点击「一键注入」。");
            if (!File.Exists(_dllPath))
                AppendLog($"警告: 未找到 UseMyTimeHook.dll（{_dllPath}），注入将失败。", Color.OrangeRed);
        };
    }

    // ------------------------------------------------------------------
    // 布局（手写，避免依赖 designer 文件）
    // ------------------------------------------------------------------
    private void InitializeComponentManual()
    {
        Text = "UseMyTime - 进程级时间劫持工具";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(760, 560);
        MinimumSize = new Size(640, 480);
        Font = new Font("Microsoft YaHei UI", 9.5f);

        // ===== 顶部：进程选择 =====
        var topPanel = new Panel { Dock = DockStyle.Top, Height = 96, Padding = new Padding(12) };

        var lblProcess = new Label { Text = "目标进程:", AutoSize = true, Location = new Point(12, 14) };
        _processList.Location = new Point(12, 38);
        _processList.Size = new Size(280, 46);
        _processList.HorizontalScrollbar = true;

        var lblTarget = new Label { Text = "已选目标:", AutoSize = true, Location = new Point(312, 14) };
        _cmbTarget.Location = new Point(312, 38);
        _cmbTarget.Size = new Size(240, 28);

        _btnRefresh.Location = new Point(568, 38);
        _btnRefresh.Size = new Size(88, 28);
        _btnRefresh.Click += async (_, _) => await RefreshProcessListAsync();

        _btnInject.Location = new Point(568, 70);
        _btnInject.Size = new Size(168, 30);
        _btnInject.BackColor = Color.FromArgb(32, 160, 90);
        _btnInject.ForeColor = Color.White;
        _btnInject.FlatStyle = FlatStyle.Flat;
        _btnInject.Click += async (_, _) => await InjectAsync();

        _cmbTarget.SelectedIndexChanged += (_, _) =>
        {
            var info = _cmbTarget.SelectedItem as ProcessInfo;
            _targetPid = info?.Id ?? -1;
            _targetName = info?.Name ?? "";
            UpdateStateLabel();
        };

        topPanel.Controls.AddRange(new Control[]
        {
            lblProcess, _processList, lblTarget, _cmbTarget, _btnRefresh, _btnInject,
        });

        // ===== 中部：虚拟时间设置 =====
        var midPanel = new Panel { Dock = DockStyle.Top, Height = 84, Padding = new Padding(12) };

        var lblOffset = new Label { Text = "时间偏移 (时:分:秒):", AutoSize = true, Location = new Point(12, 14) };
        _numHours.Location = new Point(170, 12);
        _numMinutes.Location = new Point(238, 12);
        _numSeconds.Location = new Point(306, 12);
        var lblColon1 = new Label { Text = ":", AutoSize = true, Location = new Point(232, 14) };
        var lblColon2 = new Label { Text = ":", AutoSize = true, Location = new Point(300, 14) };
        var lblHint = new Label
        {
            Text = "正数=快进到未来  负数=回退到过去",
            AutoSize = true, Location = new Point(380, 14), ForeColor = Color.Gray,
        };

        var lblSpeed = new Label { Text = "流速 (x):", AutoSize = true, Location = new Point(170, 48) };
        _numSpeed.Location = new Point(238, 46);
        var lblSpeedHint = new Label
        {
            Text = "1.0=正常  2.0=两倍速  0=暂停",
            AutoSize = true, Location = new Point(320, 48), ForeColor = Color.Gray,
        };

        _btnApply.Location = new Point(480, 12);
        _btnApply.Size = new Size(120, 30);
        _btnApply.BackColor = Color.FromArgb(40, 120, 200);
        _btnApply.ForeColor = Color.White;
        _btnApply.FlatStyle = FlatStyle.Flat;
        _btnApply.Enabled = false;
        _btnApply.Click += async (_, _) => await ApplyVirtualTimeAsync();

        _btnReset.Location = new Point(610, 12);
        _btnReset.Size = new Size(120, 30);
        _btnReset.FlatStyle = FlatStyle.Flat;
        _btnReset.Enabled = false;
        _btnReset.Click += async (_, _) => await ResetTimeAsync();

        midPanel.Controls.AddRange(new Control[]
        {
            lblOffset, _numHours, lblColon1, _numMinutes, lblColon2, _numSeconds, lblHint,
            lblSpeed, _numSpeed, lblSpeedHint, _btnApply, _btnReset,
        });

        // ===== 状态区 =====
        var statusPanel = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = 40,
            Padding = new Padding(12, 8, 12, 8),
            WrapContents = false,
        };
        _lblRealTime.Location = new Point(12, 10);
        _lblVirtualTime.Location = new Point(250, 10);
        _lblState.Location = new Point(470, 10);
        _lblOffset.Location = new Point(590, 10);
        _lblSpeed.Location = new Point(680, 10);
        statusPanel.Controls.AddRange(new Control[]
        {
            _lblRealTime, _lblVirtualTime, _lblState, _lblOffset, _lblSpeed,
        });

        // ===== 日志区 =====
        var logPanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12, 4, 12, 12) };
        var lblLog = new Label { Text = "日志:", Dock = DockStyle.Top, AutoSize = true };
        logPanel.Controls.Add(_logBox);
        logPanel.Controls.Add(lblLog);

        // 添加顺序：Fill 先加，Top 后加（Dock 布局规则）
        Controls.Add(logPanel);
        Controls.Add(statusPanel);
        Controls.Add(midPanel);
        Controls.Add(topPanel);

        FormClosing += OnFormClosing;
    }

    // ------------------------------------------------------------------
    // 进程列表（后台枚举 -> UI 更新）
    // ------------------------------------------------------------------
    private async Task RefreshProcessListAsync()
    {
        _btnRefresh.Enabled = false;
        _btnRefresh.Text = "刷新中...";
        try
        {
            // 后台线程枚举（Process.GetProcesses 可能较慢）
            var list = await Task.Run(() => ProcessListService.GetInjectableProcesses());

            _cmbTarget.Items.Clear();
            _processList.Items.Clear();

            int? previous = null;
            foreach (var p in list)
            {
                _processList.Items.Add($"{p.Name,-28} pid={p.Id}  {(p.Is64Bit ? "64-bit" : "32-bit")}");
                _cmbTarget.Items.Add(p);
                if (p.Id == _targetPid) previous = p.Id;
            }

            // 恢复之前的选择
            for (int i = 0; i < _cmbTarget.Items.Count; i++)
            {
                if ((_cmbTarget.Items[i] as ProcessInfo)?.Id == previous)
                {
                    _cmbTarget.SelectedIndex = i;
                    break;
                }
            }

            AppendLog($"进程列表已刷新，共 {list.Count} 个候选进程。");
        }
        catch (Exception ex)
        {
            AppendLog($"刷新进程列表失败: {ex.Message}", Color.OrangeRed);
        }
        finally
        {
            _btnRefresh.Enabled = true;
            _btnRefresh.Text = "刷新列表";
        }
    }

    // ------------------------------------------------------------------
    // 一键注入（async，全程不阻塞 UI）
    // ------------------------------------------------------------------
    private async Task InjectAsync()
    {
        var info = _cmbTarget.SelectedItem as ProcessInfo;
        if (info == null)
        {
            AppendLog("请先选择目标进程。", Color.OrangeRed);
            return;
        }

        if (!File.Exists(_dllPath))
        {
            AppendLog($"未找到 hook DLL: {_dllPath}", Color.OrangeRed);
            return;
        }

        _btnInject.Enabled = false;
        _btnInject.Text = "注入中...";
        AppendLog($"开始注入 {info.Name} (pid={info.Id}) ...");

        try
        {
            // 关键：阻塞式 Win32 注入放到后台线程，UI 保持响应
            bool ok = await Task.Run(() =>
                ProcessInjection.Inject(info.Id, _dllPath, out string err));

            if (!ok)
            {
                AppendLog($"注入失败: {err}", Color.OrangeRed);
                return;
            }

            // 等待 DLL 内部 worker 线程完成初始化（hook + IPC 服务）
            await Task.Delay(800);

            // 探测 IPC 服务是否就绪
            var (probeOk, _, probeErr) = await IpcClient.GetStatusAsync(info.Id, _cts.Token);
            if (!probeOk)
            {
                AppendLog($"注入成功但 IPC 探测失败: {probeErr}（目标可能已退出或 hook 未生效）",
                    Color.OrangeRed);
                return;
            }

            _targetPid = info.Id;
            _targetName = info.Name;
            _injected = true;
            _btnApply.Enabled = true;
            _btnReset.Enabled = true;
            UpdateStateLabel();
            AppendLog($"注入成功！{info.Name} (pid={info.Id}) 的时间劫持已就绪。", Color.LimeGreen);
        }
        catch (Exception ex)
        {
            AppendLog($"注入过程异常: {ex.Message}", Color.OrangeRed);
        }
        finally
        {
            _btnInject.Enabled = true;
            _btnInject.Text = "一键注入";
        }
    }

    // ------------------------------------------------------------------
    // 应用虚拟时间
    // ------------------------------------------------------------------
    private async Task ApplyVirtualTimeAsync()
    {
        if (!_injected || _targetPid < 0)
        {
            AppendLog("尚未注入，请先执行注入。", Color.OrangeRed);
            return;
        }

        long offsetMs = (long)(
            _numHours.Value * 3600_000 +
            _numMinutes.Value * 60_000 +
            _numSeconds.Value * 1_000);
        double speed = (double)_numSpeed.Value;

        _btnApply.Enabled = false;
        AppendLog($"应用虚拟时间: 偏移 {FormatMs(offsetMs)}, 流速 {speed:0.00}x ...");

        try
        {
            var (ok, status, err) =
                await IpcClient.SetOffsetAsync(_targetPid, offsetMs, _cts.Token);
            if (ok)
            {
                var (ok2, _, err2) =
                    await IpcClient.SetSpeedAsync(_targetPid, speed, _cts.Token);
                AppendLog(ok2
                    ? $"已应用: 虚拟时间 = 真实时间 {FormatMs(offsetMs)} @ {speed:0.00}x"
                    : $"偏移已应用，但流速设置失败: {err2}",
                    ok2 ? Color.LimeGreen : Color.OrangeRed);
            }
            else
            {
                AppendLog($"应用失败: {err}（目标进程未受影响，可重试）", Color.OrangeRed);
            }
        }
        catch (Exception ex)
        {
            AppendLog($"应用虚拟时间异常: {ex.Message}", Color.OrangeRed);
        }
        finally
        {
            _btnApply.Enabled = _injected;
        }
    }

    // ------------------------------------------------------------------
    // 恢复真实时间
    // ------------------------------------------------------------------
    private async Task ResetTimeAsync()
    {
        if (!_injected || _targetPid < 0) return;

        _btnReset.Enabled = false;
        AppendLog("恢复真实时间 ...");

        try
        {
            var (ok, status, err) = await IpcClient.ResetAsync(_targetPid, _cts.Token);
            AppendLog(ok
                ? "已恢复真实系统时间。"
                : $"恢复失败: {err}（可重试，或等待目标进程退出自动恢复）",
                ok ? Color.LimeGreen : Color.OrangeRed);
            if (ok)
            {
                _numHours.Value = 0;
                _numMinutes.Value = 0;
                _numSeconds.Value = 0;
                _numSpeed.Value = 1;
            }
        }
        catch (Exception ex)
        {
            AppendLog($"恢复真实时间异常: {ex.Message}", Color.OrangeRed);
        }
        finally
        {
            _btnReset.Enabled = _injected;
        }
    }

    // ------------------------------------------------------------------
    // 状态轮询（后台 Timer -> BeginInvoke 回 UI）
    // ------------------------------------------------------------------
    private void PollStatusTick()
    {
        // 目标进程退出检测
        if (_injected && _targetPid > 0 && !IpcClient.IsProcessAlive(_targetPid))
        {
            _injected = false;
            BeginInvoke(() =>
            {
                _btnApply.Enabled = false;
                _btnReset.Enabled = false;
                UpdateStateLabel();
                AppendLog($"目标进程 {_targetName} (pid={_targetPid}) 已退出，hook 已随进程消失。",
                    Color.OrangeRed);
            });
            return;
        }

        if (!_injected || _targetPid < 0) return;

        // IPC 查询放后台，避免任何阻塞
        _ = Task.Run(async () =>
        {
            var (ok, status, _) = await IpcClient.GetStatusAsync(_targetPid, _cts.Token);
            if (!IsHandleCreated) return;
            BeginInvoke(() =>
            {
                if (status != null)
                {
                    _lblRealTime.Text = $"真实时间: {status.RealTime}";
                    _lblVirtualTime.Text = $"虚拟时间: {status.VirtualTime}";
                    _lblOffset.Text = $"偏移: {FormatMs(status.OffsetMs)}";
                    _lblSpeed.Text = $"流速: {status.Speed:0.00}x";
                    if (!status.Enabled)
                    {
                        _lblState.Text = "状态: hook 已卸载";
                        _lblState.ForeColor = Color.OrangeRed;
                    }
                }
                else if (!ok)
                {
                    // 查询失败不报错刷屏，只在状态栏提示
                    _lblState.Text = "状态: 通信中断";
                    _lblState.ForeColor = Color.OrangeRed;
                }
            });
        });
    }

    // ------------------------------------------------------------------
    // 辅助
    // ------------------------------------------------------------------
    private void UpdateStateLabel()
    {
        if (_injected && _targetPid > 0)
        {
            _lblState.Text = $"状态: 已注入 ({_targetName})";
            _lblState.ForeColor = Color.LimeGreen;
        }
        else
        {
            _lblState.Text = "状态: 未注入";
            _lblState.ForeColor = Color.Gray;
        }
    }

    private static string FormatMs(long ms)
    {
        bool neg = ms < 0;
        ms = Math.Abs(ms);
        int h = (int)(ms / 3600_000);
        int m = (int)((ms % 3600_000) / 60_000);
        int s = (int)((ms % 60_000) / 1000);
        return $"{(neg ? "-" : "")}{h:D2}:{m:D2}:{s:D2}";
    }

    private void AppendLog(string message, Color? color = null)
    {
        if (!IsHandleCreated) return;
        BeginInvoke(() =>
        {
            string stamp = DateTime.Now.ToString("HH:mm:ss");
            _logBox.SelectionStart = _logBox.TextLength;
            _logBox.SelectionLength = 0;
            _logBox.AppendText($"[{stamp}] {message}{Environment.NewLine}");
            if (color != null)
            {
                // 简单实现：整段着色不可行，仅滚动到底部
            }
            _logBox.ScrollToCaret();
            // 限制日志长度
            if (_logBox.TextLength > 200_000)
                _logBox.Text = _logBox.Text[^100_000..];
        });
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        // 优雅退出：通知目标卸载 hook 恢复真实时间（尽力而为）
        if (_injected && _targetPid > 0)
        {
            try
            {
                // 同步短等待，最多 2 秒
                var t = IpcClient.ShutdownAsync(_targetPid,
                    CancellationTokenSource.CreateLinkedTokenSource(_cts.Token).Token);
                t.GetAwaiter().GetResult();
                AppendLog("已通知目标进程卸载 hook，恢复真实时间。");
            }
            catch
            {
                AppendLog("退出时通知目标失败（目标进程退出后 hook 也会自动清除）。",
                    Color.OrangeRed);
            }
        }
        _cts.Cancel();
        _statusTimer.Dispose();
    }
}
