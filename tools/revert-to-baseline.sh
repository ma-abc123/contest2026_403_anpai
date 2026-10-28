#!/usr/bin/env bash
# revert-to-baseline.sh — 一键回退到已知可用的 BLE 基线
#
# 锚点：pre-imu-burst-20261028
#   由 2026-10-28 建立，覆盖 7 个仓库。该状态下：
#     - BLE 可连、CCC 正常、timesync 正常（verify_4825.log 实机验证）
#     - E1 LCPU 补丁已入库（去掉连接路径 RFC 复位脉冲，HW error 78%→0/4）
#     - legacy 广播 + 3s 快速自愈
#     - IMU 尚无 burst 读（168% 总线占用是已知待修项）
#
# 用法：
#   ./tools/revert-to-baseline.sh                 # 只体检，不动任何文件（默认）
#   ./tools/revert-to-baseline.sh --apply         # 真的回退
#   TAG=别的tag ./tools/revert-to-baseline.sh     # 换锚点
#
# 安全设计：
#   1. 默认 dry-run，不加 --apply 绝不修改工作区。
#   2. 回退前为每个有改动的仓库建 wip-before-revert-<时间戳> 分支（坏代码不丢）。
#   3. 已跟踪但未提交的修改先 git stash push 保存，可用 git stash list 找回。
#   4. 未跟踪文件一律不碰：git reset --hard 不删除未跟踪文件，所以用不带 -u 的
#      stash、且全程不用 git clean。这条是硬约束 —— 见下面 WARNING。
#   5. 回退后提示重新构建；cmake_out/prod 属于产物，不随 git 回退。
#
# WARNING（2026-10-28 实测）：
#   vendor/openvela/boards/contest2026_403_ai_watch 是一个**未跟踪的软链**，
#   指向本作品的板级配置，构建依赖它。任何 `git stash -u` / `git clean -fd`
#   都会把它收走或删掉，导致下一次构建直接失败且报错与源码无关、极难定位。
#   同理 vendor/sifli 下的 *.bak / *.pre_e1 是刻意保留的 E1 备份，不能动。

set -uo pipefail

WS="${WS:-/home/ma/openvela-contest403}"
TAG="${TAG:-pre-imu-burst-20261028}"
STAMP="$(date +%Y%m%d-%H%M%S)"
APPLY=0

REPOS=(
  "contest2026_403_anpai"
  "nuttx"
  "external"
  "external/zblue/zblue"
  "vendor/sifli"
  "frameworks/runtimes/feature"
  "vendor/openvela"
)

for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$arg（用 --help 看用法）" >&2; exit 2 ;;
  esac
done

say()  { printf '%s\n' "$*"; }
head1() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

if [ "$APPLY" -eq 1 ]; then
  say "模式：APPLY（会修改工作区）"
else
  say "模式：DRY-RUN（只体检，加 --apply 才真正回退）"
fi
say "工作区：$WS"
say "锚点  ：$TAG"

# ---------- 第一步：体检 ----------
missing=0
head1 "体检"
for r in "${REPOS[@]}"; do
  d="$WS/$r"
  if [ ! -d "$d/.git" ] && ! git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then
    printf '  %-30s %s\n' "$r" "不是 git 仓库 / 路径不存在"
    missing=1
    continue
  fi
  if ! git -C "$d" rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    printf '  %-30s %s\n' "$r" "缺少 tag $TAG"
    missing=1
    continue
  fi
  tracked=$(git -C "$d" -c core.preloadIndex=false status --porcelain --untracked-files=no 2>/dev/null | wc -l)
  untracked=$(git -C "$d" -c core.preloadIndex=false status --porcelain 2>/dev/null | grep -c '^??' || true)
  ahead=$(git -C "$d" rev-list --count "$TAG"..HEAD 2>/dev/null || echo '?')
  printf '  %-30s tag=%s  HEAD=%s  领先锚点=%s  已跟踪改动=%s  未跟踪=%s\n' \
    "$r" "$(git -C "$d" rev-parse --short "$TAG")" \
    "$(git -C "$d" rev-parse --short HEAD)" "$ahead" "$tracked" "$untracked"
done

if [ "$missing" -eq 1 ]; then
  say ""
  say "有仓库缺少锚点 tag。若这是首次在别的机器上回退，先确认 tag 是否随 clone 带过来："
  say "  git -C <repo> tag -l '$TAG'"
  say "（tag 默认不会被 git push 带走，需要显式 git push origin <tag>）"
fi

if [ "$APPLY" -eq 0 ]; then
  head1 "DRY-RUN 结束"
  say "以上未修改任何文件。确认无误后执行："
  say "  $0 --apply"
  exit 0
fi

# ---------- 第二步：保命 ----------
head1 "保命（建分支 + stash）"
SKIP=()
for r in "${REPOS[@]}"; do
  d="$WS/$r"
  git -C "$d" rev-parse --git-dir >/dev/null 2>&1 || continue
  tracked=$(git -C "$d" -c core.preloadIndex=false status --porcelain --untracked-files=no 2>/dev/null | wc -l)
  if [ "$tracked" -gt 0 ]; then
    br="wip-before-revert-$STAMP"
    if git -C "$d" branch "$br" 2>/dev/null; then
      printf '  %-30s 已建分支 %s\n' "$r" "$br"
    fi
    if git -C "$d" stash push -m "before-revert-$TAG-$STAMP" >/dev/null 2>&1; then
      printf '  %-30s 已 stash（%s 个已跟踪改动）\n' "$r" "$tracked"
    else
      printf '  %-30s stash 失败 → 跳过该仓，不回退\n' "$r"
      SKIP+=("$r")
      continue
    fi
  fi
done

# ---------- 第三步：回退 ----------
head1 "回退"
fail=0
for r in "${REPOS[@]}"; do
  d="$WS/$r"
  git -C "$d" rev-parse --git-dir >/dev/null 2>&1 || continue
  skip=0
  for s in "${SKIP[@]:-}"; do [ "$s" = "$r" ] && skip=1; done
  if [ "$skip" -eq 1 ]; then
    printf '  %-30s 跳过（保命步骤未成功）\n' "$r"
    fail=1
    continue
  fi
  if git -C "$d" reset --hard "$TAG" >/dev/null 2>&1; then
    printf '  %-30s OK  -> %s\n' "$r" "$(git -C "$d" log --oneline -1)"
  else
    printf '  %-30s 失败\n' "$r"
    fail=1
  fi
done

# ---------- 第四步：复核 ----------
head1 "复核"
for r in "${REPOS[@]}"; do
  d="$WS/$r"
  git -C "$d" rev-parse --git-dir >/dev/null 2>&1 || continue
  d1=$(git -C "$d" diff --name-only "$TAG" | wc -l)
  d2=$(git -C "$d" -c core.preloadIndex=false status --porcelain --untracked-files=no | wc -l)
  printf '  %-30s tag↔HEAD=%s  未提交=%s\n' "$r" "$d1" "$d2"
  [ "$d1" -eq 0 ] || fail=1
done

head1 "下一步"
say "1) 必须重新构建（产物不回退）："
say "     cd $WS && ninja -C cmake_out/prod"
say "2) 若要找回被回退的坏代码："
say "     git -C <repo> branch --list 'wip-before-revert-*'"
say "     git -C <repo> stash list"
say "3) 若回退后仍有问题，说明问题不在本次改动，另找原因，不要反复回退。"
say ""

if [ "$fail" -eq 0 ]; then
  say "结论：全部仓库已回到锚点 $TAG。"
else
  say "结论：存在不一致，请按上面输出逐个核对。" >&2
  exit 1
fi
