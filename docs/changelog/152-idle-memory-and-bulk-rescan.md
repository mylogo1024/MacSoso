# 152: 后台常驻内存压缩与批量重扫提速

## 背景

2026-10-26 维护者反馈：MacSoSo 在后台静默常驻时占用的内存过高（比
Windows 版 Everything 还高），搜索速度也偏慢。

用生产规模索引（约 510 万条记录 / 477 万条存活 / 68.5 万个唯一目录）
实测，优化前引擎自身画像为 **3309.1 MiB**，`footprint -p` 报告的脏页为
**3243 MB**。逐项定位后确认三个问题：

1. `pathIndex_`、`pathLookup_`、`lowerPathLookup_` 三个映射都是
   `std::unordered_map<std::string, uint32_t>`，把整条路径字符串在哈希桶里
   又复制了一份。仅 `pathIndex_` 就占 933 MiB，而它本可以通过
   FNV-1a 哈希 + 回查字符串池来定位记录。
2. 增量更新让 SoA 列与字符串池按 2 倍扩容，空闲时仍保留最多 2 倍的
   冗余容量；`shrink_to_fit` 式的收缩又会在下一次追加时立刻重新翻倍，
   收缩必须带余量。
3. 目录级批量删除（`removeByPathPrefix` / `batchRescanPrefix`）对每条记录
   单独在 trigram 倒排链里做 `erase`，链是有序的、删除是线性搬移，
   5 万条子树删除退化成近似平方复杂度；测试里一次重扫耗时 3607 ms。

## 改动

### 路径索引改为开放寻址哈希表

- 新增 `MacSoSo/Core/PathIndexTable.h`：16 字节槽（`uint64` 哈希 +
  `uint32` 值）、Fibonacci 散列、负载因子 2/3、后移删除（无墓碑槽）。
  表内不保存路径文本，命中后由调用方提供的 `verify` 回调回查字符串池，
  因此哈希冲突不会把两个不同文件混为一谈。
- `PathIndexTable` 增加分段喂入的哈希接口（`hashSeed` / `hashFeed` /
  `hashPath`），目录 + `/` + 文件名可以直接算出整条路径的哈希，批量加载
  不再为每条记录拼一个 `std::string`。
- `SearchEngine` 的 `pathIndex_`、`pathLookup_`、`lowerPathLookup_` 全部换成
  `PathIndexTable`；新增 `loweredPathHashUnlocked()` 直接从字符串池折叠哈希，
  `buildPathIndexUnlocked()` 并行算哈希、串行插入，
  `rebuildPathLookupsUnlocked()` 取代原来的手工重建。
- `compactRecords()` 的快照不再复制整张 `pathIndex_`，改为用
  `forEach()` 生成一个 winner 位图；重放删除时先在 tombstone 之前取好哈希，
  再用 `eraseByHash()` 摘除。
- 查询侧（`SearchEngineQuery.cpp`、`SearchEngineAdvancedQuery.cpp`、
  `SearchEngineStructuredQuery.cpp`）的子目录查找改走
  `findPathIdxUnlocked()` / `findLowerPathIdxUnlocked()`。

### 倒排链批量删除

- 新增 `SearchEngine::removeRecordsFromIndexesBatch()`：先收集受影响的
  trigram / pathIdx / 扩展名键，排序去重后，对每条受影响的倒排链只做一次
  线性过滤（`eraseSortedFrom`），空链直接删除。把平方级搬移换成
  「每条链最多走一遍」。
- `removeByPathPrefix` 与 `batchRescanPrefix` 的第一阶段改为先
  `collectSubtreeRecordsUnlocked()` 收集、再批量摘除索引、最后逐条写 WAL
  并 tombstone。

### 空闲容量回收

- `StringPool::shrinkToFit()` 换成 `shrinkWithSlack(slackBytes)`，
  `SearchEngine::trimCapacity()` 改为带余量收缩：目标容量为
  `size + max(size/16, slack)`，只有当前容量明显超过目标才真正重分配。
  字符串池留 4 MiB 追加余量，SoA 列留 6.4 万条，倒排链留 8～16 个元素。
- `trimCapacity()` 额外收缩 `nameTrigramIndex_`、`pathTrigramIndex_`、
  `extensionIndex_` 中每条倒排链的容量（Phase 2 构建时它们都是按倍扩容）。
- `IndexPersistence` 的首次自动压缩 tick 从 30 s 延后到 60 s，避免刚启动、
  Phase 2 还在跑时就抢锁收缩；tick 内继续 `flush()` + `trimCapacity()`。

### 内存画像

- `SearchEngine::memoryProfile()`（`/api/status?debug=memory`）按池、列、
  路径索引、倒排链、扩展名索引等逐项报告容量，并新增
  `hot-name-trigrams` / `hot-path-trigrams` 两行，用于判断「丢弃高频
  trigram」这类裁剪是否划算。

## 效果

同一份生产规模索引，引擎画像与进程脏页：

| 阶段 | 引擎画像 TOTAL | `footprint` 脏页 |
|------|---------------|-----------------|
| 优化前基线 | 3309.1 MiB | 3243 MB |
| `pathIndex_` 换成 `PathIndexTable` | 2422.9 MiB | — |
| 路径查找表同样换表 + 倒排链裁剪 | 2302.0 MiB | — |
| 带余量收缩 + 首次 tick 延后 | 1556.3 MiB | 1722～1848 MB |

分项变化（同一索引规模）：

- `path-index` 933 → 128 MiB，`path-lookup` + `lower-path-lookup`
  247 → 32 MiB。
- 一次收缩 tick 内：`name-trigrams` 532.2 → 356.8 MiB，
  `path-to-records` 49.6 → 33.9 MiB，`extension-index` 19.3 → 15.3 MiB。
- 字符串池与 SoA 列的 2 倍冗余容量收敛到 1.06 倍左右。

批量重扫（`tests/test_batch_rescan.h`，约 5 万文件子树）：
旧实现 3607 ms → 新实现 30 ms 量级；测试断言
「batchRescanPrefix 不比旧方式慢」由失败转为通过。

查询耗时（同一索引，`limit=100`）：

| 查询 | 冷 | 热（结果缓存命中） |
|------|----|-------------------|
| `file2` | 2.28 ms | ~0 ms |
| `*.sh` | 13.24 ms | ~0 ms |
| `/Volumes/mac/project_new/hide_root *.sh` | 4.29 ms | 1.76 ms |
| `macsoso` | 0.18 ms | ~0 ms |
| `ext:pdf` | 1.47 ms | ~0 ms |
| `xxxyyyzzz`（无结果） | 0.03 ms | ~0 ms |

## 已知代价与后续空间

- 收缩 tick 在 510 万条记录上会独占写锁约 280 ms（实测一次查询最大延迟
  286 ms，p99 仍为 2.5 ms）。它只在启动后第一个 tick 明显，之后每轮只有
  少量链需要重分配。
- 引擎画像 1556 MiB 与 `footprint` 1848 MB 之间约 280 MB 的差值是
  libmalloc 小块空闲页：实测 `malloc_zone_pressure_relief()`（含显式
  zone 与 size 提示）对这类残留无效，因此没有保留该调用。
- 剩余最大项是 trigram 倒排链（`name-trigrams` + `path-trigrams`
  约 655 MiB）。高频 trigram 裁剪只值约 165 MiB 且会拖慢常见词查询，
  不划算；真正值得做的是把倒排链改写成单一 arena 上的
  delta + varint（CSR）存储，预计可再省 300～400 MiB，但会触及所有查询
  热路径与增量更新路径，需要单独立项。

## 验证

- `make test-fast`：12166 项断言全部通过。
- `make app` Release 构建成功；`macsoso-daemon` 在临时缓存副本上加载真实
  生产索引，路径 + 扩展名查询、`ext:` 查询、无结果查询均返回正确。
- 全程使用 `/tmp/macsoso_memtest` 的索引副本测量，未改动
  `~/Library/Caches/com.macsoso.app` 正式缓存。
