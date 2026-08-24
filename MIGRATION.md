# AI Watch 迁移与开发说明

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

## 磁盘处理

新工作区同步曾因磁盘空间不足而中止。旧工作区删除前必须确认：

- 专属仓的 `app/ai_watch/` 与 `board/ai_watch/` 文件完整。
- `patches/` 中存在 apps 4 个、vendor-sifli 3 个、frameworks-runtimes-feature 1 个补丁。
- `docs/development/` 中的规划和开发日志完整。
- `migration-backups/` 中的三个 Git bundle 均通过 `git bundle verify`。
- 专属仓迁移提交已经创建，最好已经推送到个人 Fork。

释放旧工作区空间后，再在 `/home/ma/openvela-contest403` 运行 `repo sync -c -j4`。同步中断可以续传，不需要重新执行 `repo init`。
