# 148: 项目更名为 MacSoSo

## 背景

项目此前沿用上游名称 MacEverything。2026-10-23 按维护者要求，将当前项目
与产品统一更名为 **MacSoSo**，同时保留对上游
[joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything) 的
署名与 MIT 版权声明。

## 命名映射

| 项目 | 旧名称 | 新名称 |
|------|--------|--------|
| 产品 / 应用 | MacEverything | MacSoSo |
| Xcode 工程 | MacEverything.xcodeproj | MacSoSo.xcodeproj |
| App target / scheme | MacEverything | MacSoSo |
| MCP target / 可执行文件 | MacEverythingMCP | MacSoSoMCP |
| UI 测试 target | MacEverythingUITests | MacSoSoUITests |
| Bundle ID | com.maceverything.app | com.macsoso.app |
| 源码目录 | MacEverything/ | MacSoSo/ |
| 桥接头 / entitlements | MacEverything-* | MacSoSo-* |
| CLI 守护进程 | maceverything-daemon | macsoso-daemon |
| 缓存目录 | ~/Library/Caches/com.maceverything.app | ~/Library/Caches/com.macsoso.app |
| 日志目录 / 文件 | ~/Library/Logs/MacEverything | ~/Library/Logs/MacSoSo |
| 日志文件 | maceverything.log | macsoso.log |
| MCP 配置键 | maceverything | macsoso |
| DMG | MacEverything-<ver>-<arch>.dmg | MacSoSo-<ver>-<arch>.dmg |

## 具体改动

- 用 `git mv` 重命名工程、源码目录、UI 测试目录、入口 Swift 文件、桥接头
  与 entitlements，避免丢失 Git 历史。
- `project.pbxproj` 中 target、scheme、product、group path、远程引用与
  Bundle ID 全部更新；UI 测试使用 `com.macsoso.app.uitests`。
- SwiftUI 窗口标题、菜单项、快捷键说明、MCP 配置管理器与 MCP 初始化响应
  改用 MacSoSo。
- 核心层缓存/日志路径、dispatch queue 标签、日志轮转文件名同步更新；
  FSEvents 排除路径也改为新缓存目录。
- Makefile、`scripts/package_release.sh`、GitHub Actions 与测试中的二进制
  名、路径与断言同步更新。
- README 中英文版、项目说明、发布文档、基准报告与竞品分析改用新名称。

## 保留项

- 上游项目名、仓库链接、基线提交、MIT 版权声明与 `NOTICE` 中的改编关系。
- voidtools Everything 的名称仅用于描述搜索语法兼容性。
- 编号 148 之前的历史 changelog 保留当时的名称与文件路径，并在文件头加上
  改名说明。

## 升级影响

- Bundle ID 与缓存目录变化：新版本首次启动会重建索引，旧缓存不会复用；
  `UserDefaults` 中的搜索历史与快捷键设置也会重置。
- 已写入 Claude Code / Cursor / Claude Desktop 的 MCP 配置项由
  `maceverything` 变为 `macsoso`，需要在应用菜单中重新添加。
- 若旧版本安装到 `/Applications`，建议删除旧 App 及旧登录项，避免重复启动。
- 旧缓存与日志保留在原路径（`com.maceverything.app`、
  `~/Library/Logs/MacEverything`），确认无用后可自行删除。

## 验证

- 重命名后 `make test-fast`：12162 个断言通过，0 失败。
- Xcode 工程可列出 `MacSoSo`、`MacSoSoMCP`、`MacSoSoUITests` 三个 target。
- Release 构建产物 `MacSoSo.app`：Bundle ID `com.macsoso.app`、名称
  `MacSoSo`、版本 1.3.0 (4)，ad-hoc 签名校验通过，无 Homebrew 残留依赖。
- 打包产物 `dist/MacSoSo-1.3.0-arm64.dmg`（2.7 MB），SHA256 校验通过；
  DMG 内含 `MacSoSo.app`、`Applications` 链接及 `Licenses/` 署名文件。
- 运行新 App 后在真实索引（约 472 万条记录）上验证
  `/Volumes/mac/project_new/hide_root *.sh` 返回 5 个结果，耗时约 35ms。
- MCP 服务 `MacSoSoMCP` 的 initialize 响应名称为 `MacSoSo`。
