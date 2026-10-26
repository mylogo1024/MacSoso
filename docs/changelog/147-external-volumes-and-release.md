# 147: 外置卷索引、发布流水线与改编声明

> **历史记录：** 本条目完成时项目名为 MacEverything，产出的 DMG 也使用旧名；
> 项目于 2026-10-23 更名为 MacSoSo，当前发布流程的产物名为 `MacSoSo-*.dmg`。

## 背景

用户报告 `/Volumes/mac/project_new/hide_root *.sh` 查询不到文件。引擎和
查询语法测试均正常，问题出在索引覆盖范围：`DirectoryScanner` 只遍历根设备，
挂载在 `/Volumes/*` 的其他本地卷（例如本机 APFS 外置卷 `dev=16777241`）
被当作跨设备目录跳过。旧缓存又掩盖了这次缺失，FSEvents 只对启动后的新增
文件生效。

同时为后续推送到 GitHub、在线构建 Release 做准备：需要可复现的打包脚本、
GitHub Actions 工作流，以及明确的上游改编声明和改动说明。

## 外置卷修复

- `DirectoryScanner::isIndexableVolume()` 新增卷筛选策略：
  - 仅接受 `/Volumes/` 下的挂载点；
  - 要求 `MNT_LOCAL` 且不含 `MNT_DONTBROWSE`；
  - 文件系统限于 `apfs`、`hfs`、`exfat`、`msdos`、`ntfs`、`udf`、`cd9660`。
- `discoverUserVolumes()` 在扫描开始调用 `getmntinfo(MNT_NOWAIT)`，用
  `stat()` 取得额外设备 ID；扫描目录时允许这些设备，网络盘、autofs、
  devfs、系统 firmlink 仍然跳过。
- `FlatIndexWriter::kVersion` 从 1 升到 2，使不含外置卷的旧 `index.v6`
  自动失效并触发完整重扫，避免继续使用不完整缓存。
- `tests/test_scanner_cancel.h` 增加卷策略断言：APFS/exFAT/FAT 可索引；
  系统 firmlink、SMB/NFS、隐藏卷、非本地挂载被拒绝。

验证：重建索引后记录数从约 452 万增加到约 471 万；用户查询返回 5 个
`.sh` 文件。

## 发布流水线

- `scripts/package_release.sh`：
  - Release 构建，`RE2_PREFIX` / `ABSEIL_PREFIX` 支持 CI 覆盖；
  - 递归内嵌 re2/abseil 动态库，改写 `@rpath` / `@loader_path`；
  - 校验无残留 Homebrew 绝对路径；
  - 默认 ad-hoc 签名，可传 Developer ID；
  - 生成 DMG（含许可证）和 SHA256。
- `.github/workflows/release.yml`：
  - `v*` 标签或手动触发；
  - arm64 (`macos-26`) 与 x86_64 (`macos-26-intel`) 双架构构建；
  - 先跑 `make test-fast`，再并行打包，最后创建 GitHub Release。
- Xcode 工程的 Homebrew 路径改为 `$(RE2_PREFIX)` / `$(ABSEIL_PREFIX)`，
  默认仍指向 `/opt/homebrew`，Intel runner 由脚本传入 `/usr/local` 前缀。
- Makefile 增加 `make dmg` / `make release`，`make app` 自动选择 Xcode。

## 声明与文档

- 新增 `NOTICE`：上游 joshua-wu/MacEverything、基线提交 `26f0332`、
  非 voidtools 隶属关系、修改摘要。
- 新增 `docs/CHANGES_FROM_UPSTREAM.md`：按搜索语义、界面、外置卷、
  工程化分类的完整改动说明。
- 新增 `THIRD_PARTY_NOTICES.md` 与 `third_party/re2-LICENSE`、
  `third_party/abseil-LICENSE`；DMG 内附 `Licenses/` 目录。
- README 中英文增加改编声明、架构化下载说明、源码构建与签名提示。

## 测试与验证

- `./test_all --fast`：12162 passed / 0 failed。
- `./test_all --part 80`：54 passed / 0 failed。
- `package_release.sh --arch arm64 --version 1.3.0` 成功生成 DMG；
  `codesign --verify --deep --strict` 通过，无外部绝对依赖路径。
- 实机 API 验证用户查询返回预期文件。
