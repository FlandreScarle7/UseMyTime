#!/usr/bin/env bash
# ============================================================================
# UseMyTime - Git 仓库初始化脚本 (Shell, macOS / Linux / Git-Bash)
#
# 用法（在项目根目录执行）:
#   ./scripts/init_git.sh
#
# 可选环境变量:
#   REMOTE_URL  远程地址（默认 https://github.com/YOUR_GITHUB_USER/UseMyTime.git）
#   BRANCH      分支名（默认 main）
# ============================================================================
set -euo pipefail

REMOTE_URL="${REMOTE_URL:-https://github.com/YOUR_GITHUB_USER/UseMyTime.git}"
BRANCH="${BRANCH:-main}"
COMMIT_MSG="${COMMIT_MSG:-chore: initial UseMyTime project (hook DLL + WinForms app + CI)}"

# 切到项目根目录（脚本所在目录的上一级）
cd "$(dirname "$0")/.."

echo "==> UseMyTime Git 初始化"
echo "    目录: $(pwd)"

# --- 0. 检查 git ---
if ! command -v git >/dev/null 2>&1; then
    echo "错误: 未找到 git，请先安装。" >&2
    exit 1
fi

# --- 1. 初始化 ---
if [ -d .git ]; then
    echo "==> 仓库已存在，跳过 init"
else
    git init -b "$BRANCH" 2>/dev/null || { git init && git checkout -b "$BRANCH"; }
    echo "==> 已初始化仓库 (分支: $BRANCH)"
fi

# --- 2. 本地用户配置（避免影响全局）---
git config user.name  >/dev/null 2>&1 || git config user.name  "UseMyTime Dev"
git config user.email >/dev/null 2>&1 || git config user.email "dev@usemytime.local"

# --- 3. 远程 ---
if git remote get-url origin >/dev/null 2>&1; then
    echo "==> origin 已存在: $(git remote get-url origin)"
else
    git remote add origin "$REMOTE_URL"
    echo "==> 已添加 origin: $REMOTE_URL"
fi

# --- 4. 提交 ---
git add -A
if [ -n "$(git status --porcelain)" ]; then
    git commit -m "$COMMIT_MSG"
    echo "==> 已提交"
else
    echo "==> 无变更，跳过提交"
fi

# --- 5. 推送 ---
echo "==> 尝试推送到 origin/$BRANCH ..."
if git push -u origin "$BRANCH" 2>/dev/null; then
    echo "==> 推送成功"
else
    echo "==> 推送失败（远程可能不存在或无权限）。"
    echo "    请先在 GitHub 创建仓库，再手动执行: git push -u origin $BRANCH"
fi

echo ""
echo "==> 完成。最近提交:"
git log --oneline -5 || true
git status
