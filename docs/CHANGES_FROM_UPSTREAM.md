# 相对上游 MacEverything 的改编说明

本仓库改编自 [joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything)，
上游基线为提交 `26f0332`。上游以 MIT 许可证发布，本仓库保留同一许可证，
并在根目录 `NOTICE` 中注明改编关系。本项目与 voidtools 无隶属关系，
"Everything" 仅用于说明搜索语法兼容性。

> **命名说明**：本仓库当前名称为 **MacSoSo**，于 2026-10 由 MacEverything
> 更名而来。本文中的“上游”始终指 joshua-wu/MacEverything；第 1–5 节的
> 技术改动在更名前完成，相关源码路径现已位于 `MacSoSo/`。

以下按主题列出相对上游基线的主要改动。

## 1. 搜索语义对齐 voidtools Everything

查询解析器、匹配器与排序逻辑按 Everything 1.5 的公开文档重写：

- **词法/布尔**：空格为 AND，`|` 为 OR，`!` 为 NOT，`< >` 分组；OR 的
  优先级高于 AND（`a b | c` = `a AND (b OR c)`）；双引号内为字面文本；
  `( )`、`AND`、`OR`、`NOT` 按普通文件名文本处理。
- **通配符与路径词**：`*` / `?` 不跨 `/`，`**` 跨目录，`\` 规范为 `/`；
  裸词匹配文件名，含 `/` 的词匹配完整路径；`~/` 会展开；结尾 `/` 限制为
  该目录的直接子项。
- **过滤器/函数**：`ext:`、`noext:`、`name:`、`stem:`、`extension:`、
  `full-path:`、`path:`、`nopath:`、`location:` / `loc:`、`parent:`、
  `parent-name:`、`ancestor:`、`ancestor-name:`、`len:`、`size:`、
  `fs:`、`depth:`、`name-length:`、`full-path-length:`、`child*:`、
  `empty:`、`dupe*:`、`content:` / `infile:`、媒体类型宏、`count:`、
  `distinct:`、`unique:`。
- **修饰符**：`case:`、`nocase:`、`regex:`、`wildcards:`、`no-wildcards:`、
  `whole:` / `wfn:` / `fn:` / `fu:`、`whole-words:` / `ww:`、
  `ignore-punctuation:`、`ignore-whitespace:`、`diacritics:`、
  `dot-all:`、`multi-line:`、`ungreedy:`、`prefix:` / `suffix:`、
  `start-with:` / `end-with:`、`?path:` 全局路径匹配。
  其中 `prefix:` / `suffix:` 按 Everything 的定义匹配文件名的开头/结尾。
- **日期**：`dm:` / `dc:` / `da:` 支持相对区间（`today`、`last7days`、
  `10mins` 等）和分段过滤（`dm-hour:`、`dm-month:`、`dm-year:`、
  `dm-dow:` 等）。
- **排序**：`sort:name;size-descending;-path` 多列、逐列方向；
  `folders-first:` 先分组再截断；未指定排序时保持相关度顺序。
- **引擎执行**：路径三元组预筛、聚合后处理、排序在 `maxResults` 截断前
  完成；新增查询结果缓存与相关测试。

## 2. 界面与交互对齐 Everything

- 四列结果表：Name / Path / Size / Date Modified，列标题点击切换排序，
  并通过 `sort:` 修饰符交给引擎排序。
- 扁平工具栏：搜索框 + 常驻 `Aa` / `W` / `P` / `.*` / `FN` 选项开关。
- 底部状态栏：结果数、选中数、内容索引进度、实时监控、索引总数、耗时。
- 移除 100 条分页，表格直接显示完整结果集并虚拟化滚动。
- 搜索框语法高亮、语法帮助窗口、行右键菜单、拖放、双击打开。
- 索引扫描期间保留已有结果，只在无结果时显示索引进度。

## 3. 外置卷覆盖修复

用户反馈 `/Volumes/mac/project_new/hide_root *.sh` 查询不到文件。根因是
`DirectoryScanner` 只遍历根设备，挂载在 `/Volumes/*` 的其他本地卷被跳过。

- 新增 `DirectoryScanner::isIndexableVolume()`：仅接受 `/Volumes/` 下的
  本地、可浏览卷，文件系统限于 APFS/HFS/exFAT/FAT/NTFS/UDF/ISO。
- 扫描开始时通过 `getmntinfo` + `stat` 发现额外设备 ID，允许遍历这些卷。
  网络盘、autofs、devfs、系统 firmlink、`MNT_DONTBROWSE` 卷仍然跳过。
- Flat 索引格式版本从 1 升到 2：旧缓存会因版本不匹配而失效，下次启动
  自动完整重建，避免继续使用缺少外置卷数据的旧索引。
- 新增卷筛选策略单元测试，并用真实外置卷查询验证。

## 4. 发布与工程化

- 新增 `scripts/package_release.sh`：Release 构建、递归打包 re2/abseil
  动态库、重写 `@rpath`/`@loader_path`、签名、生成 DMG 与 SHA256。
- 新增 `.github/workflows/release.yml`：推送 `v*` 标签或手动触发后，
  在 Apple Silicon 与 Intel 两个 macOS runner 上测试、构建并发布两个
  架构的 DMG。
- Xcode 工程中的 Homebrew 路径改为 `$(RE2_PREFIX)` / `$(ABSEIL_PREFIX)`，
  便于 Intel（`/usr/local`）与 CI 覆盖。
- 新增 `NOTICE`、`THIRD_PARTY_NOTICES.md` 与第三方许可证副本。

## 5. 验证

- `./test_all --fast`：12162 个断言通过，0 失败。
- 在真实索引（约 471 万条记录）上验证：
  `/Volumes/mac/project_new/hide_root *.sh` 返回 5 个 `.sh` 文件。
- Debug App 通过 Xcode 26.1 构建并在本机运行验证。

## 6. 项目更名（MacEverything → MacSoSo）

2026-10 将项目与产品统一更名为 **MacSoSo**，涉及：

- Xcode 工程：`MacSoSo.xcodeproj`，targets/schemes 为 `MacSoSo`、
  `MacSoSoMCP`、`MacSoSoUITests`；源码目录 `MacSoSo/`；桥接头文件与
  entitlements 同步改名。
- 应用身份：Bundle ID 改为 `com.macsoso.app`，UI 测试改为
  `com.macsoso.app.uitests`；窗口标题、菜单、快捷键提示与 MCP 初始化
  响应使用 MacSoSo。
- 运行路径与接口：缓存 `~/Library/Caches/com.macsoso.app`、日志
  `~/Library/Logs/MacSoSo`、日志文件 `macsoso.log`、dispatch queue 标签
  与 MCP 配置键 `macsoso` 全部更新。
- 工具链：CLI 守护进程改为 `macsoso-daemon`，MCP 可执行文件改为
  `MacSoSoMCP`；Makefile、打包脚本、GitHub Actions 与产出的 DMG 名称
  同步更新。
- 保留项：上游 `joshua-wu/MacEverything` 的署名、链接、基线提交与 MIT
  版权声明；voidtools Everything 的语法兼容性描述；历史 changelog 中的
  旧名称与旧路径（作为历史记录）。
- 升级影响：Bundle ID 与缓存路径变化会触发一次全量重建；已配置的 MCP
  工具需要改用新的 `macsoso` 服务项。

## 7. 应用图标

2026-10-23 使用维护者提供的新图标源图替换 AppIcon 资源：

- 源图保存在 `assets/appicon/macsoso_icon.png`。
- `scripts/generate_app_icons.py` 会移除与画布边缘相连的白色背景，按
  macOS 1024 px 图标网格居中排布，并生成 AppIcon 所需的全部尺寸。
- 替换 `MacSoSo/Assets.xcassets/AppIcon.appiconset/` 中的
  `icon_16.png` 至 `icon_1024.png`；README 图标与构建后的 `AppIcon.icns`
  会随之更新。

## 8. 发布仓库与首次版本

2026-10-26 将本地项目根目录切换为
`/Volumes/mac/project_new/Everything/MacSoso`，并将首次公开发布版本调整为
`v1.0.0`：

- 当前远端仓库为 `https://github.com/mylogo1024/MacSoso.git`。
- `Info.plist` 使用 `1.0.0 (1)`；GitHub Actions 的 Release 标签也必须先
  推送到远端，`workflow_dispatch` 只负责选择已有标签，不会自动创建标签。
- Release 工作流新增 `preflight` 任务，会在构建前校验标签是否存在，并给出
  缺失标签时的修复命令。
