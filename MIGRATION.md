# AI Watch 迁移与开发说明

## 迁移状态

- 2026-08-24：AI Watch 作品代码、板级配置、开发文档和公共仓补丁已迁入专属参赛仓。
- 2026-08-24：迁移提交已推送至个人 Fork 的 `contest/ai-watch` 分支。
- 远程备份提交：`10071eef9bbccb51f9950dfad2a55b4ee2f5d3f3`。
- 远程分支：<https://github.com/ma-abc123/contest2026_403_anpai/tree/contest/ai-watch>
- 该分支目前仅为个人 Fork 中的开发备份，尚未向组委会仓库发起或合入参赛 PR。
- 2026-08-25：新工作区已完成 `repo sync`，AI Watch 的应用和板级配置软链接已生效。

## 唯一工作区

后续只在以下目录开发：

```text
/home/ma/openvela-contest403
```

作品仓位于：

```text
/home/ma/openvela-contest403/contest2026_403_anpai
```

作品源码以专属参赛仓为准，不再直接把新增作品文件提交到公共 `apps` 或 `vendor/sifli` 仓库。

## 目录映射

`contest2026_403_anpai.xml` 定义以下映射：

| 专属仓源目录 | openvela 编译树目录 |
| --- | --- |
| `app/ai_watch/` | `apps/examples/ai_watch/` |
| `board/ai_watch/` | `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch/` |

修改应用和 defconfig 时，直接编辑专属仓中的源目录。编译树中的映射路径不作为提交位置。

## 本机 Manifest 配置

本工作区的 `.repo/local_manifests/contest403.xml` 会覆盖组委会模板中的默认 `hello_app` 映射，并固定专属仓到当前已验证的提交：

```text
f67e3110898b63e5496c9ced70568cd703490fdf
```

该文件仅作用于本机工作区，不提交到参赛仓。它避免后续 `repo sync` 将映射还原为模板目录。

以后参赛仓产生并推送新的提交后，如需让 `repo sync` 固定到新版本，应将此文件中的 `revision` 更新为新的完整提交号，再运行：

```bash
cd /home/ma/openvela-contest403
repo sync -l contest2026_403_anpai
```

正常日常开发不需要执行这条命令；只在修改本机 manifest 或需要重新生成软链接时使用。

## 公共仓改动

旧工作区中的 Git 提交已完整导出到 `patches/`，用于保留历史和恢复公共仓修改：

- `patches/apps/`：旧应用提交的完整历史备份。应用源码已迁入 `app/ai_watch/`，正常情况下不要再应用这些补丁。
- `patches/vendor-sifli/0001-*`：旧板级 defconfig 提交备份。defconfig 已迁入 `board/ai_watch/`，正常情况下不要应用此补丁。
- `patches/vendor-sifli/0002-*` 和 `0003-*`：修改现有 SiFli 启动文件，需要恢复到新工作区的 `vendor/sifli` 分支，并最终向公共仓提交 PR。
- `patches/frameworks-runtimes-feature/`：通用编译修复，需要恢复到新工作区的 `frameworks/runtimes/feature` 分支，并最终向公共仓提交 PR。

原始提交对象另存于工作区本机的 `migration-backups/*.bundle`。这些 bundle 不上传，用于补丁无法应用时精确恢复原分支。`docs/development/legacy-manifest-lock.xml` 保存了旧工作区各项目的准确基线版本。

完成 `repo sync` 后，公共仓补丁可分别恢复：

```bash
cd /home/ma/openvela-contest403/vendor/sifli
git switch -c contest/ai-watch openvela/dev-ai-contest-2026
git am ../../contest2026_403_anpai/patches/vendor-sifli/0002-*.patch \
       ../../contest2026_403_anpai/patches/vendor-sifli/0003-*.patch

cd /home/ma/openvela-contest403/frameworks/runtimes/feature
git switch -c contest/ai-watch openvela/dev-ai-contest-2026
git am ../../../contest2026_403_anpai/patches/frameworks-runtimes-feature/*.patch
```

应用补丁前先确认目标分支没有同名本地分支，也没有包含等价的上游修复。

## 日常提交边界

- 作品应用、作品专用配置、README、开发文档和 AI 日志：提交到 `contest2026_403_anpai`。
- 对现有公共源码文件的修改：提交到对应公共仓分支，并向 `dev-ai-contest-2026` 发起 PR。
- 编译产物、下载缓存、密钥和无关测试文件：不提交。

`repo status` 可能显示以下本机状态，均不应提交到公共源码仓：

- `.claude/settings.local.json`：本机 AI 工具权限设置。
- `apps/examples/ai_watch`：从参赛仓映射出的应用软链接。
- `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch`：从参赛仓映射出的板级配置软链接。
- `apps/testing/drivers/nist-sts`：上游嵌套仓检出状态提示；本次同步已完成，不要为消除提示而手工删除其文件。

因此不要在 `apps/` 或 `vendor/sifli/` 下执行 `git add .`。应用和 defconfig 的提交只在 `contest2026_403_anpai/` 目录完成。

## 磁盘处理

新工作区同步曾因磁盘空间不足而中止。旧工作区删除前必须确认：

- 专属仓的 `app/ai_watch/` 与 `board/ai_watch/` 文件完整。
- `patches/` 中存在 apps 4 个、vendor-sifli 3 个、frameworks-runtimes-feature 1 个补丁。
- `docs/development/` 中的规划和开发日志完整。
- `migration-backups/` 中的三个 Git bundle 均通过 `git bundle verify`。
- 专属仓迁移提交已经创建，最好已经推送到个人 Fork。

释放旧工作区空间后，再在 `/home/ma/openvela-contest403` 运行 `repo sync -c -j4`。同步中断可以续传，不需要重新执行 `repo init`。
