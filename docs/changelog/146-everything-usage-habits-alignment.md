# 146: 搜索习惯对齐 voidtools Everything

> **历史记录：** 本条目完成时项目名为 MacEverything，文中的 `MacEverything/`
> 路径对应现在的 `MacSoSo/`；项目于 2026-10-23 更名为 MacSoSo。

## 背景

用户要求把 MacEverything 的搜索使用习惯对齐到 voidtools Everything 1.5
（参考 https://www.voidtools.com/support/everything/ 的 Search Syntax /
Search Modifiers / Search Functions 三篇文档）。本变更按 Everything 的真实
语义实现查询解析、匹配、聚合与排序，并同步帮助、README 与测试。

## 语义要点

- **默认匹配文件名**：裸词只在文件名中匹配。含 `/`（或 `\`）的词按完整
  路径子串匹配；结尾 `/` 限制为某目录的内容。
- **通配符**：`*` / `?` 不跨越 `/`，`**` 可跨目录；`\` 统一规范为 `/`。
- **布尔运算**：空格为 AND，`|` 为 OR，`!` 为 NOT，`< >` 为分组；
  Everything 的优先级是 OR 先于 AND，即 `a b | c` = `a AND (b OR c)`。
  `AND` / `OR` / `NOT` 关键字与括号 `( )` 都是普通文件名文本。
- **可叠加修饰符**：`case:regex:^test_`、`whole:` / `wfn:` / `fn:` / `fu:`
  等按链式作用于同一词；`?path:` 为全局修饰符，作用于整条查询。
- **路径过滤器**：`path:`（完整路径+文件名）、`location:` / `loc:`（父路径）、
  `parent:`（精确父目录）、`full-path:`、`parent-name:`、`ancestor:`、
  `ancestor-name:`、`nopath:`、`depth:`。
- **长度过滤器**：`len:`、`name-length:` / `namelen:`、
  `full-path-length:` / `fullpathlen:`。
- **聚合过滤器**：`empty:` 仅匹配索引中没有子项的空目录；`childcount:`、
  `child-file-count:`、`child-folder-count:`、`child:` / `child-file:` /
  `child-folder:`；`dupe:` 按属性分组查重，`dupe-min:` / `dupe-max:` 限制
  组大小，`dupe-count:` 限制每组显示条数，`dupe-from:` 保留包含指定文件夹
  （含子目录，支持通配符）成员的整个重复组。
- **日期**：`dm:` / `dc:` / `da:` 支持 `today`、`last7days` 等滚动区间，
  以及 `dm-month:`、`dm-hour:` 等日期段过滤器。
- **排序 `sort:`**：支持 `name/path/size/ext/type/dm/dc/da/depth`，多列用
  `;` 分隔（`sort:size;name`），每列可单独指定方向
  （`sort:name;size-descending;-path`），`folders-first:` 可单独使用或作为
  列选项。排序在 `maxResults` 截断之前完成，`folders-first:` 同样先分组
  再截断。未指定 `sort:` 时保持相关度排序。
- **参数保留大小写**：`case:ext:CPP` 等区分大小写的扩展名匹配不再被提前
  小写化。

## 界面

主窗口按 Everything 的布局重做：

- **四列表格**：Name / Path / Size / Date Modified，紧凑行高与图标；列标题
  可点击切换方向，排序通过 `SearchOptions.buildQuery` 回写成 `sort:`
  修饰符，保证引擎排序发生在 `maxResults` 截断之前。
- **底部状态栏**：左侧“N objects”与选中数量，右侧内容索引进度、实时监控、
  索引文件总数与查询耗时。
- **扁平工具栏**：搜索框（13pt）+ 常驻选项开关 `Aa` / `W` / `P` / `.*` / `FN`，
  激活时高亮；移除旧的分页底栏与彩色徽章。
- **行操作**：双击打开，右键 Open / Open Path / Copy Full Name(s)，保留拖放。
- **完整结果集**：移除 100 条分页，表格直接渲染引擎一次返回的全部结果并
  虚拟化滚动。
- **高亮缓存**：`highlightHints` 在查询完成时解析一次并发布，滚动时不再
  重复解析查询。

## 变更文件

| 文件 | 变更 |
|------|------|
| `MacEverything/Core/QueryTokenizer.h` | 新过滤器名、修饰符链、`< >` 分组词法 |
| `MacEverything/Core/QueryParser.cpp` | OR 先于 AND、`< >` 分组、排序/结果修饰符提取 |
| `MacEverything/Core/QueryFilterParser.h` | 修饰符叠加、文本函数、聚合过滤器、排序控制保留 |
| `MacEverything/Core/QuerySort.h` | 多列排序、逐列方向、`folders-first` |
| `MacEverything/Core/SearchEngine.h` | 排序/聚合接口 |
| `MacEverything/Core/SearchEngineAdvancedQuery.cpp` | 求值、路径 Trigram 预筛、聚合后处理、排序截断 |
| `MacEverything/Core/SearchEngineQuery.cpp` / `V6` / `Index.cpp` | 配套更新 |
| `MacEverything/Core/HighlightHintExtractor.h` | 完整路径词拆分 PATH/NAME 高亮提示 |
| `MacEverything/App/ContentView.swift` | Everything 风格四列表格、列标题排序、扁平工具栏、底部状态栏、行操作 |
| `MacEverything/App/ResultRow.swift` | 表格单元格格式化（大小/日期）、图标缓存、打开/定位/复制操作 |
| `MacEverything/App/SearchViewModel.swift` | 完整结果集、高亮缓存、选项与排序变化后的刷新 |
| `MacEverything/App/SearchOptions.swift` | Match Path 选项（`?path:`）、`sort:` 修饰符、常驻选项开关 |
| `MacEverything/App/HighlightedSearchField.swift` | 13pt 搜索框、过滤器补全列表更新 |
| `MacEverything/App/MacEverythingApp.swift` | Everything 式菜单命名与 Match Path 菜单项（⇧⌘P） |
| `MacEverything/App/SearchSyntaxHelpView.swift` | 语法帮助按最终语义重写 |
| `README.md` / `README_EN.md` | 查询示例、过滤器与排序表更新 |
| `tests/test_everything_compat.h` | 排序、dupe、路径、修饰符兼容性测试 |
| `tests/test_trigram_competition.h` / `test_structured_query.h` / `test_tilde_expansion.h` / `test_preprocess_unified.h` | 语义修正后的期望值 |

## 测试结果

- `make test_all`：12074 passed / 8 failed；8 个失败均为本机环境下的
  FSEvents / E2E 监听用例（`Events received: 0`），与查询语义无关。
- 新增兼容性用例覆盖：多列排序与逐列方向、`folders-first:` 截断窗口、
  `dupe-min:` / `dupe-max:` / `dupe-count:` / `dupe-from:`、长名称长度
  过滤器、`path-part` 与完整路径词。
- Swift 界面部分已用 `swiftc -typecheck` 对全部 App 源码做类型检查，退出
  码为 0（仅存既有并发警告）。本机 `xcode-select` 指向 CommandLineTools；
  命令行构建需显式设置
  `DEVELOPER_DIR=/Applications/Xcode_26.1.app/Contents/Developer`。
