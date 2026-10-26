# 153: 倒排链改为单块 arena 的 delta + varint 压缩存储

## 背景

152 之后，生产规模索引（515 万条记录 / 480 万条存活 / 69 万个唯一目录）
的引擎画像降到 1556 MiB、`footprint` 脏页 1818～1848 MB，但最大项仍然是
trigram 倒排链：`name-trigrams` 356.8 MiB + `path-trigrams` 约 298 MiB
+ `extension-index` 15.3 MiB ≈ 670 MiB，占画像的 43%。

先量了一下这些字节的构成，结论和直觉不同：

- 实际不同的 trigram 只有 **8.8 万个**（name）/ **4.5 万个**（path），
  所以「每个 key 一个容器的固定开销」根本不是瓶颈；
- 瓶颈是 payload 本身：每条链是 `std::vector<uint32_t>`，每个 posting 固定
  4 字节，再叠加按倍扩容留下的容量冗余；
- 高频链裁剪（丢弃 df > 2% 的 trigram）只值 62 + 103 MiB，且会让常见词查询
  退化成线性扫描，152 已经判定不划算。

因此把三张倒排表换成「单块字节 arena + delta + LEB128 varint」的 CSR 存储。

## 改动

### 新增 `MacSoSo/Core/PostingIndex.h`

`PostingIndex<Key>`（`Key` 为 `Trigram` 或扩展名 `std::string`）：

- **存储**：`arena_`（一个字节块，存全部 varint 编码的 delta 链）、
  `descs_`（每链 12 字节描述符 `{offset, bytes, count}`）、
  `ids_`（key → 描述符下标）、`delta_`（构建之后的增量层）。
- **`build(Owned&&)`**：逐链排序去重后写入 arena，**编码完一条立刻
  `swap` 释放源链**，所以「未压缩的 map」和「完整 arena」不会同时驻留。
- **读**：`decode()` 合并 base + delta（合并后 `unique` 一次，保证同一个
  记录号不会重复出现在候选集里）；`count()` 给出合并后的基数；
  `intersectWith()` 边解码 base 游标、边遍历 delta，与传入的有序候选集求交，
  **不物化被查 key 的整条链**——对一条两百万 posting 的高频 trigram 做探测
  是零分配。
- **写**：`add()` / `remove()` / `removeMany()` 只作用于 delta。arena 是变长
  编码，无法就地删元素；被删记录的 posting 留在 arena 里，由查询循环中既有
  的 `types_[idx] == 0` 墓碑过滤兜住（这是本改动正确性的前提，已逐点核对：
  `SearchEngineAdvancedQuery.cpp`、`SearchEngineStructuredQuery.cpp`、
  `SearchEngineQuery.cpp` 的候选循环都有该过滤）。
- **`foldDelta()`**：把增量折回 arena。没有增量的链按字节原样拷贝，
  有增量的链 decode + 重新编码，delta-only 的 key 变成新的 base 链。
  没有这一步，长期运行的进程会把「每 key 一个 vector」的开销重新长回来。
- **`trim()` / `memoryBytes()` / `hotPostings()` / `basePostings()`**：
  arena 是一整块，收缩会把真实页面还给内核；后两个供内存画像使用。
- varint 游走去掉逐字节越界检查：链由 `build()` / `foldDelta()` 写入，
  结尾必然是终止字节，`*p & 0x80` 的循环不需要 `p < end`。配合下面的止损
  规则，`package.json`（12117 候选）的 trigram 阶段实测 4.4 → 0.9 ms。

### 接入 `SearchEngine`

- `nameTrigramIndex_`、`pathTrigramIndex_`、`extensionIndex_` 换成
  `PostingIndex<...>`；`pathIdxToRecords_` 保持 `vector<vector<uint32_t>>`
  不变（只有 69 万条链，且查询要按 pathIdx 直接下标取）。
- `intersectPostingLists()` / `intersectPostingListsMulti()` 重写：先用
  `count()` 找最稀有的 trigram，`decode()` 它作为基底，其余 trigram 用
  `intersectWith()` 逐条收窄；任一 trigram 不存在仍然返回 `allFound = false`。
- **收窄止损规则**（实测后加上的，否则 trigram 阶段会退化）：varint 链不支持
  随机访问，收窄一条链的代价是把它整条解码一遍（实测约 2 ns/posting），而
  多留一个候选的代价只是候选循环里的一次校验（实测约 20 ns/record，
  `package.json` 12099 候选 → phase2 0.27 ms）。所以按 count 升序收窄时，
  一旦 `count(t) > 16 × 当前候选数` 就停止：
  由于 `count(t) ≤ totalSize`，跳过收窄必然意味着候选数 < `totalSize/16`，
  仍满足调用方 `≤ totalSize/10` 的门槛，不会因为止损而退回线性扫描。
  代价是候选集从「全部 trigram 的精确交集」变成「超集」——结果仍然由既有的
  逐条校验保证精确，实测每个查询的 `resultCount` 与旧实现一致。
- Phase 2（`completePhase2()`）与 COW 压缩（`compactRecords()`）的 arena 编码
  放在**取写锁之前**完成，锁内只做 move 交换，避免把编码耗时算进查询停顿。
- 构建后的增量都写进 delta：`addTrigramsForRecord()`、
  `ensurePathTrigramsForPathIdx()`、`addExtensionForRecord()`；
  删除走 `removeTrigramsForRecord()` / `removeRecordsFromIndexesBatch()`。
- `trimCapacity()` 删掉了逐链 `shrink_to_fit` 与三张表的 `rehash(0)`
  （现在只剩 3 个 arena 和 3 张 key 表），改为
  「delta ≥ 262144 条，或 ≥ base 的 1/4」时触发 `foldDelta()`，再 `trim()`。
- `estimateTrigramCost()` 改用 `count()`；DIAG 日志的 `indexBuckets` 改为
  `indexKeys`；画像里 `name-trigrams` / `path-trigrams` / `extension-index`
  按 `memoryBytes()` 统计，并新增 `name-trig-delta`、`path-trig-delta` 两行。

### 测试

- 新增 `tests/test_posting_index.h`（Part 81，52 条断言），已接入
  `make test-fast`：编解码往返、重复值合并、空链丢弃、宽/窄 delta
  （含 `{0, UINT32_MAX}`）、压缩率断言、delta 合并与增删、重建清空陈旧
  delta、`trim()` 后仍正确、`hotPostings()` 统计、string key 形态，以及
  `foldDelta()` 的「无增量链逐字节保留 / 增量落到有序位置 / delta-only key
  变成 base 链 / 撤销的增量不会写进 arena」，以及一轮生产形状的批量折叠：
  2000 个 key × 300 条 posting，20000 次伪随机增删维护一份逐 key 参考链，
  折叠后每个 key 的 `decode()` 与参考链**逐元素相等**，`intersectWith()`
  的探测结果与 `std::set_intersection` 一致。

## 效果

同一份生产规模索引（`macsoso-daemon` 加载 `/tmp/macsoso_memtest` 的索引副本）：

| 项 | 152 之后 | 本次 |
|----|----------|------|
| 引擎画像 TOTAL | 1556.3 MiB | **1092.5 MiB** |
| `footprint -p` 脏页 | 1818～1848 MB | **1168 MB** |
| `name-trigrams` | 356.8 MiB | 115.5 MiB |
| `path-trigrams` | 约 298 MiB | 82.0 MiB |
| `extension-index` | 15.3 MiB | 5.1 MiB |
| `hot-name-trigrams` | 62 MiB / 73 链 | 11.9 MiB / 73 链 |
| `hot-path-trigrams` | 103 MiB / 153 链 | 26.0 MiB / 153 链 |

倒排三项合计 670 → 202.6 MiB（压缩到约 30%，即平均每个 posting 约
1.5 字节），进程脏页比 `Everything` Windows 版的量级更接近预期。

止损规则加上之后重新起一次 daemon，再跑 5 分钟真实 FSEvents 增量（副本 +
WAL 回放，记录数 517.9 万）：画像 TOTAL 1094.6 MiB、`phys_footprint` 1281 MB，
其中 `name-trig-delta` 10189、`path-trig-delta` 797 条增量尚未折叠（远低于
触发阈值）；启动期峰值 `phys_footprint_peak` 2938 MB，出现在 Phase 2 建完、
收缩 tick 跑完之前。

Phase 2 后台建索引（517 万条记录）：17:07:21.354 开始 → 17:07:29.211 建完
→ 17:07:29.293 交换完成，共 7.9 s，其中锁内耗时 82 ms（含 arena 编码之后的
move 交换与增量回放）。arena 编码本身在锁外完成。

查询耗时：同一台机器上并行跑两个进程做 A/B——旧二进制（152 的 GUI app，
读正式索引）与新二进制（`macsoso-daemon`，读索引副本），`limit=100`，
每次请求换 `limit` 绕过结果缓存后取 9 次里的最小值/中位数：

| 查询 | 旧 min/med (ms) | 新 min/med (ms) | 结果数 旧/新 |
|------|-----------------|-----------------|-------------|
| `xxxyyyzzz`（无结果） | 0.00 / 0.01 | 0.00 / 0.00 | 0 / 0 |
| `zsh_history` | 0.02 / 0.03 | 0.03 / 0.04 | 2 / 2 |
| `kernelcache` | 0.02 / 0.07 | 0.06 / 0.09 | 19 / 19 |
| `macsoso` | 0.03 / 0.06 | 0.02 / 0.07 | 83 / 82 |
| `libsystem` | 0.07 / 0.10 | 0.06 / 0.10 | 119 / 119 |
| `file2` | 0.07 / 0.14 | 0.03 / 0.18 | 88 / 111 |
| `ext:pdf` | 0.17 / 0.27 | 0.13 / 0.19 | 1000 / 1000 |
| `*.sh` | 0.22 / 0.76 | 0.51 / 0.75 | 1000 / 1000 |
| `*.swift` | 0.69 / 0.89 | 0.77 / 1.15 | 1000 / 1000 |
| `package.json` | 1.50 / 2.19 | 2.03 / 2.64 | 1000 / 1000 |
| `/Volumes/mac/project_new/hide_root *.sh` | 3.67 / 4.30 | 4.18 / 4.77 | 5 / 5 |

两个进程各有实时 FSEvents 增量在跑，0.1 ms 量级的差异落在噪声里（同一组
查询在不同轮次里 min/med 会互相翻转）；可以确认的是加了止损规则之后，
trigram 预筛阶段不再有肉眼可见的退化——未加时 `package.json` 的
`trigramMs` 是 4.4 ms，旧实现同期为 1.5 ms。

## 已知代价

- **删除不再真正缩小 arena**。墓碑记录的 posting 会留在 arena 里，直到下一次
  `foldDelta()`（新增文件）或 compaction 重建（重编号）才消失；在此期间
  `estimateTrigramCost()` 与候选计数会偏高，可能让个别查询多扫一些候选。
  结果集不受影响（候选循环过滤 `types_[idx] == 0`）。
- `foldDelta()` 在写锁内做一次 arena 拷贝 + 受影响链重编码，按当前规模约
  几十毫秒，只在增量攒到阈值的那一个 tick 发生。
- `build()` 仍要先构建 `unordered_map<Trigram, vector<uint32_t>>` 中间结构，
  峰值是「中间结构 + arena」。实测（152 记录里担心的小块脏页）没有留下明显
  残留：脏页 1168 MB 已低于画像 1092.5 MiB 之外的常驻开销。启动峰值
  `phys_footprint_peak` 2938 MB，出现在 Phase 2 建完、收缩 tick 跑完之前。
  若要进一步压低这个峰值，需要改成基于 24 位 trigram 直接计数数组的两趟
  CSR 构建。
- **收窄变成有损预筛**。止损规则跳过的收窄让候选集变超集：实测
  `libsystem` 候选 119 → 232、`ContentIndex` 22 → 1375、`test_query`
  44 → 373，但结果数不变（逐条校验兜底），耗时反而更短。

## 验证

- `make test-fast`：12218 项断言全部通过（12166 + 本次新增 52）。
- 默认全集（含 Part 1/4/6）：仅 Part 4「FSEvents Integration」与 Part 6
  「E2E（FSEvents → SearchEngine）」的 8 项失败，原因是这两个用例监视
  `/tmp/macsoso_test_<pid>`，而本机 FSEvents 对 `/tmp`（指向
  `/private/tmp` 的符号链接）投递 0 条事件；这两项自始与倒排存储无关，
  Part 4 完全不使用 `SearchEngine`。
- 新旧二进制双进程 A/B（旧 GUI app 读正式索引 : 19860，新 `macsoso-daemon`
  读索引副本 : 19862，`limit=1000`）：13 组查询里 11 组的**结果路径集合逐条
  相同**（`ext:pdf`、`*.sh`、`*.swift`、`package.json`、`/Volumes/mac/
  project_new/hide_root *.sh`、`libsystem`、`kernelcache`、`zsh_history`、
  `xxxyyyzzz`、`test_query`、`brew sh`），`searchPath` 与 `candidates` 也一致；
  只有 `file2`（88 → 111）与 `macsoso`（83 → 82）不同，原因是副本取自
  16:40，此后正式索引又多收录/删除了记录（旧进程 `totalRecords`
  5 175 880，新进程 5 163 070），不是索引结果差异。
- 全程使用 `/tmp/macsoso_memtest` 的索引副本，未改动
  `~/Library/Caches/com.macsoso.app` 正式缓存。
- `AddressSanitizer`（`make test-asan`）跑完整 fast 套件：12218 项通过，
  无越界报告——用来确认去掉 varint 逐字节越界检查之后，游标仍然只在
  终止字节处停住。
- **待补**：`make app` / `make dmg` 未在本次会话内重跑。本机 `xcodebuild`
  在 agent 的 shell 上下文里被立即 SIGKILL（连 `xcodebuild -version` 也是
  rc=137、无任何输出、无崩溃报告；同一目录里自编译的小程序可正常执行，
  签名为 valid），因此发布包需要在维护者自己的终端里执行
  `make app && make dmg`。本次验证走的是同一批 core 目标文件链接的
  `macsoso-daemon`，查询路径与内存画像代码完全一致。另外当前正在运行的
  GUI 进程就是 `build/DerivedData/.../Release/MacSoSo.app`（152 的旧二进制，
  本次 A/B 的基线），重新打包前可以先退出它。
