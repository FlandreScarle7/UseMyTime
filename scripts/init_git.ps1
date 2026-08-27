# ============================================================================
# UseMyTime - Git 仓库初始化脚本 (PowerShell, Windows)
#
# 用法（在项目根目录执行）:
#   powershell -ExecutionPolicy Bypass -File scripts\init_git.ps1
#
# 功能:
#   1. 初始化 git 仓库（若已存在则跳过）
#   2. 添加 origin 远程（可选，默认指向 GitHub 模板）
#   3. 添加所有文件并提交
#   4. 推送 main 分支（若配置了远程）
# ============================================================================
[CmdletBinding()]
param(
    [string]$RemoteUrl = "https://github.com/YOUR_GITHUB_USER/UseMyTime.git",
    [string]$Branch = "main",
    [string]$CommitMessage = "chore: initial UseMyTime project (hook DLL + WinForms app + CI)"
)

$ErrorActionPreference = "Stop"
Set-Location -Path (Split-Path -Parent $PSScriptRoot)  # 切到项目根目录

Write-Host "==> UseMyTime Git 初始化" -ForegroundColor Cyan
Write-Host "    目录: $(Get-Location)"

# --- 0. 检查 git 可用 ---
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "未找到 git，请先安装: https://git-scm.com/download/win"
}

# --- 1. 初始化仓库 ---
if (Test-Path ".git") {
    Write-Host "==> 仓库已存在，跳过 init" -ForegroundColor Yellow
} else {
    git init -b $Branch
    Write-Host "==> 已初始化仓库 (分支: $Branch)" -ForegroundColor Green
}

# --- 2. 默认分支与用户配置（仅本地，避免影响全局）---
git config user.name  | Out-Null 2>$null
if (-not (git config user.name))  { git config user.name  "UseMyTime Dev" }
if (-not (git config user.email)) { git config user.email "dev@usemytime.local" }

# --- 3. 添加远程 ---
$existingRemote = (git remote get-url origin 2>$null)
if ($existingRemote) {
    Write-Host "==> origin 已存在: $existingRemote" -ForegroundColor Yellow
} else {
    git remote add origin $RemoteUrl
    Write-Host "==> 已添加 origin: $RemoteUrl" -ForegroundColor Green
}

# --- 4. 暂存并提交 ---
git add -A
# 仅在有变更时提交
if ((git status --porcelain) -ne $null -and (git status --porcelain).Count -gt 0) {
    git commit -m $CommitMessage
    Write-Host "==> 已提交" -ForegroundColor Green
} else {
    Write-Host "==> 无变更，跳过提交" -ForegroundColor Yellow
}

# --- 5. 推送（远程可达时才执行）---
Write-Host "==> 尝试推送到 origin/$Branch ..." -ForegroundColor Cyan
if (git push -u origin $Branch 2>&1) {
    Write-Host "==> 推送成功" -ForegroundColor Green
} else {
    Write-Host "==> 推送失败（远程可能不存在或无权限）。" -ForegroundColor Yellow
    Write-Host "    请先在 GitHub 创建仓库，再手动执行: git push -u origin $Branch" -ForegroundColor Yellow
}

Write-Host "`n==> 完成。当前状态:" -ForegroundColor Cyan
git log --oneline -5
git status
