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

## 9. 应用图标二次替换

2026-10-26 按维护者提供的新源图（256×256 橙色放大镜，去掉上一版的蓝色
圆球）重新生成 AppIcon 全部尺寸，源图保存在
`assets/appicon/macsoso_icon.png`，构建脚本与流程沿用条目 149 的
`scripts/generate_app_icons.py` 与 `make icons`。

## 10. 常驻内存与批量重扫性能

上游把路径索引保存为 `unordered_map<std::string, uint32_t>`，后台常驻时
内存显著高于预期；目录级批量删除又在有序倒排链上逐条 `erase`。2026-10-26
的改造（详见 changelog 条目 152）：

- 新增 `MacSoSo/Core/PathIndexTable.h`：16 字节槽的开放寻址表，槽内只存
  哈希与值，命中后回查字符串池验证；`pathIndex_`、`pathLookup_`、
  `lowerPathLookup_` 全部改用该表，并支持目录 + 文件名分段喂入哈希，
  批量加载不再拼接整条路径字符串。
- 新增 `SearchEngine::removeRecordsFromIndexesBatch()`，把子树删除从
  「每条记录在每条链里线性搬移」改为「每条受影响链过滤一遍」，
  5 万文件子树重扫从约 3.6 s 降到约 30 ms。
- `trimCapacity()` 改为带余量收缩（池 4 MiB、列 6.4 万条、倒排链 8～16 个
  元素），并新增对 trigram / 扩展名倒排链容量的回收；自动压缩首次 tick 由
  30 s 延后到 60 s。
- 新增 `SearchEngine::memoryProfile()`，通过
  `/api/status?debug=memory` 输出分项容量，便于持续跟踪。
- 实测同一份约 510 万条记录的生产索引：引擎画像 3309 MiB → 1556 MiB，
  进程脏页 3243 MB → 约 1.7～1.8 GB；冷查询 0.03～13 ms，热查询命中结果
  缓存约 0 ms。
- 未采纳：高频 trigram 倒排链裁剪（只值约 165 MiB 且拖慢常见词查询）、
  `malloc_zone_pressure_relief()`（实测无法归还小块空闲页）。

## 11. 倒排链 arena 压缩

上游的三张倒排表是 `unordered_map<Key, vector<uint32_t>>`：每个 posting 固定
4 字节，再加按倍扩容留下的容量冗余和每 key 一个小块分配。条目 152 做完路径
索引与容量回收后，这三项仍占引擎画像的 43%（约 670 MiB）。2026-10-26 的改造
（详见 changelog 条目 153）：

- 新增 `MacSoSo/Core/PostingIndex.h`：全部 posting 以 delta + LEB128 varint
  编进单块字节 arena，key 只映射到 12 字节描述符 `{offset, bytes, count}`；
  构建时逐链编码并即时释放源链，编码峰值不同时持有「中间 map + 完整 arena」。
- `nameTrigramIndex_`、`pathTrigramIndex_`、`extensionIndex_` 换成
  `PostingIndex<>`。构建之后的实时增删写进 delta 层，Phase 2 完成与 COW
  压缩在**取写锁之前**把中间 map 编码成 arena，锁内只做 move 交换；
  `trimCapacity()` 在增量攒到阈值（≥ 262144 条，或 ≥ 基准的 1/4）时调用
  `foldDelta()` 把增量折回 arena，避免常驻进程把每 key 一个 vector 的开销
  重新长回来。
- 查询侧只物化最稀有的那条链，其余 trigram 用 `intersectWith()` 边解码边求交
  （探测一条两百万 posting 的链零分配），并加了「收窄代价 > 16 倍收益即止损」
  的规则：varint 链不能随机访问，整链解码约 2 ns/posting，而多留一个候选只需
  约 20 ns 校验，多余的收窄对常见关键词几乎不减少候选。删除的 posting 留在
  arena 里，由候选循环既有的 `types_[idx] == 0` 墓碑过滤兜住。
- 实测同一份约 517 万条记录的生产索引：引擎画像 1556.3 MiB → 1092.5 MiB，
  倒排三项 670 → 202.6 MiB（平均每 posting 约 1.5 字节），进程脏页
  1818～1848 MB → 1168 MB。
- 未采纳：基于 24 位 trigram 直接计数数组的两趟 CSR 构建（只影响启动峰值，
  需要重写 Phase 2 与压缩路径）；不同 key 数量级远小于预期（name 仅 8.0 万、
  path 仅 4.5 万个 trigram），所以「扁平 key 表 + 稠密 CSR」省不出多少内存。

## 12. 搜索结果过多时不再卡死

上游的写入与压缩路径在结果集很大时会把界面拖成分钟级无响应。2026-10-26 依据
系统 microstackshot 定位后做了四处改动（详见 changelog 条目 154）：

- `IndexWAL::append()` 不再每条 `fflush`，条目留在 stdio 缓冲里攒到
  `syncInterval_` 才 `fflush` + `fsync`；新增 `flushPending()` 只把缓冲推给内核、
  不做 `fsync`；`sync()` 与 `close()` 补上 `fsync` 之前的 `fflush`。
- `batchMutate()`、`removeByPathPrefix()`、`batchRescanPrefix()` 在索引独占锁内
  只做索引变更与 WAL 追加，出锁后统一 `flushPending()`——原来每条记录一次
  `write()` 全排在锁里，把所有查询都堵在 FSEvents 写入方后面。
- 压缩给搜索让路：`SearchEngine` 增加 `activeQueries_` 计数与 `hasActiveQuery()`
  （`ActiveQueryGuard` 放在结果缓存查找之后，缓存命中不让压缩等待），
  `IndexPersistence` 的定时器 tick 发现有查询在飞时延后 20 s 重试，最多 6 轮，
  避免约 672 MiB 的快照深拷贝加 640 MB 底文件重写撞上用户搜索。
- `IndexPersistence::flush()` 在「WAL 是唯一持久记录」的三个跳过分支显式
  `flushPending()`；崩溃窗口内最多丢一个在途批次 / ≤8 KB，由启动时按
  `lastEventId` 重放 FSEvents 兜底。
- 前端单飞：`SearchViewModel` 的搜索收拢为 `performSearch → enqueueSearch →
  startSearch`，索引刷新与输入产生的新请求在忙时只保留最新一条并在完成后回放，
  不再叠加多份「10000 条结果 + 逐条转行对象」。实测 SwiftUI `Table` 本身
  100→10000 行渲染耗时基本不变（46～50 ms），所以改动集中在锁与并发，而不是列表控件。
- 回归：`tests/test_wal_batch_fsync.h` 固化了缓冲/排空契约，
  `tests/test_fsevents_batch.h` 新增「不关 WAL 也能立刻读到批量变更条目」和
  `hasActiveQuery()` 两项断言；`./test_all --fast` 与 `make test-asan` 均
  12233 passed / 0 failed。

## 13. 推送即自动构建并发布

上游仓库没有发布流水线。本项目的 `.github/workflows/release.yml` 最初（条目 147、
150）要求 Release 标签事先存在，`git push` 后 CI 直接报错退出。2026-10-26 改为
「推 main 就发版」（详见 changelog 条目 155）：

- `on.push` 同时监听 `branches: [main]` 与 `tags: ["v*"]`，另有
  `workflow_dispatch.inputs.ref` 可指定分支 / 标签 / 提交。
- 新增 `preflight` job 推导发布身份：分支推送时标签取自该提交
  `MacSoSo/Info.plist` 的 `CFBundleShortVersionString`（即 `v$(plist_version)`），
  因此**抬版本号就是发布开关**；同时校验 `vX.Y.Z` 格式，并把不合法版本号挡在
  两台机器构建之前。
- `should_publish` 门控：标签已存在且指向别的提交时只跑测试、不发布，并以
  `::notice title=No release::` 提示需要 bump 版本；同版本号反复推 main 不会
  覆盖已发布的 Release。
- `test`（`make test-fast`）每次推送都跑；`build` 走 arm64 / x86_64 矩阵，
  产物 `shasum -a 256 -c` 自检后经 `actions/upload-artifact@v4` 上传。
- `publish` **先建标签再发布**：测试与两个 DMG 全部成功才 `git tag -a` 并推送，
  构建失败不会留下指向坏提交的标签；已有标签指向别的提交时拒绝改指（不
  `--force`）。发布用 `gh release view` 判存后走 `upload --clobber` + `edit`
  或 `create`，所以 dispatch 重跑同一提交是幂等的。
- 机器人自建标签不会再触发一次 workflow（GitHub 对 token 产生的事件不递归触发）。
- 发布说明固定包含两个架构的 DMG 与 SHA256、ad-hoc 签名/未公证的首次启动提示、
  Full Disk Access，以及改编自 [joshua-wu/MacEverything](https://github.com/joshua-wu/MacEverything) (MIT) 的署名。
