# 154: 搜索结果过多时界面卡死的四处修复

## 背景

2026-10-26 维护者反馈：**匹配文件很多的时候界面会卡死**。

先把前端嫌疑逐个量掉，结论是它们都不是元凶：

- SwiftUI `Table` 的行数几乎不影响渲染耗时：100 → 10000 行稳定在 46～50 ms；
- 单行高亮 0.004～0.048 ms，一万行也不到 0.5 s，而且是懒加载；
- 行对象构造、`FileItem` 数组拷贝相对 `limit` 变化可忽略；
- 主线程不做任何引擎加锁操作（Swift 侧只读原子计数与内容索引计数）。

真正的证据来自系统采样的 microstackshot
（`/Library/Logs/DiagnosticReports/MacSoSo_2026-10-26-115726_Mac-mini.diag`）
加上机器状态：16 GB 内存、swap 已用 20.4/21.5 GB、空闲仅 63 MB，而维护者当时
跑的还是 1.88 GB 常驻的旧构建。在这种饱和状态下，主线程一旦被阻塞就是分钟级
转圈。定位到四个叠加因素：

1. **FSEvents 写入路径在索引独占锁里做磁盘写。** `IndexWAL::append()` 每条记录
   都 `fflush` → `write()`，而调用方（`batchMutate`、`removeByPathPrefix`、
   `batchRescanPrefix`）全程持 `unique_lock`。一次目录批量变更几百条，就是几百次
   系统调用排在所有查询前面——搜索卡住首先是被写入方堵在锁上。
2. **自动压缩和搜索抢跑。** `IndexPersistence` 的 GCD 定时器不看用户是否在搜索，
   `fullCompact()` 要 `snapshotForV6()` 深拷贝约 672 MiB，再 `fullRewrite` 写 640 MB
   底文件（采样里弄脏 2147 MB 页、峰值 RSS 3794 MB）。
3. **结果集在飞期间被反复重跑。** `performSearch()` 用 `Task.detached` 派发，
   不受 `searchTask?.cancel()` 管辖；索引刷新与输入触发可以叠出多份
   「10000 条结果 + 逐条转 `FileItem` + 交给 `@Published`」同时跑。
4. 常驻内存本身过大（153 已解决，但需要重新打包才在维护者机器上生效）。

## 改动

### `IndexWAL`：批量写入 + 显式排空

- `append()` 去掉每条 `fflush`。条目留在 stdio 缓冲里，只有攒到 `syncInterval_`
  才 `fflush` + `fsync`（顺序很关键：`fsync` 只能刷已经交给内核的字节）。
- 新增 `flushPending()`：把缓冲推给内核，但不 `fsync`。批量变更调用方在一批结束
  后调它一次，就不用再为每条记录付一次 `write()`。
- `sync()` 与 `close()` 在 `fsync` 前补上 `fflush`，保证「显式要持久化」时缓冲里
  的条目真的在磁盘上。
- 语义变化（已在头文件注释里写明）：进程崩溃时最多丢「一个在途批次 / ≤8 KB」的
  WAL。内核页缓存不受进程退出影响，且启动路径会从 `lastEventId` 起重放 FSEvents，
  因此这段窗口有兜底。

### 批量变更改为出锁后排空 WAL

`batchMutate()`、`removeByPathPrefix()`、`batchRescanPrefix()` 统一成同一个形状：
`unique_lock` 作用域里只做索引变更和 `wal_->append()`，出锁之后再
`wal->flushPending()`。锁内一次系统调用都不剩。

### 压缩给搜索让路

- `SearchEngine` 增加 `mutable std::atomic<uint32_t> activeQueries_` 与
  `hasActiveQuery()`；`SearchEngineQuery.cpp` 里的 RAII `ActiveQueryGuard` 放在
  **结果缓存查找之后**，所以只有真正走索引的查询才计数，缓存命中的秒回不值得
  让压缩等。
- `IndexPersistence` 的 tick 回调发现 `hasActiveQuery()` 为真时，本轮直接
  `rescheduleTimer(kQueryBusyRetrySec = 20s)`，最多延后 `kMaxDeferredTicks = 6`
  轮（120 s 上限，避免搜索一直忙导致压缩永不执行）。
- `IndexPersistence::flush()` 的三个「只更新 WAL、不重写底文件」跳过分支——不脏、
  条目数未到 `kCompactThreshold`、以及非强制刷新时 WAL 未达 `kWALFoldSizeThreshold`
  ——现在都先调 `wal_->flushPending()`：这些路径里 WAL 是唯一持久记录，
  不能让它在 stdio 缓冲里过夜。（`!wal_` 和强制刷新时「WAL 只有文件头」两条分支
  本来就没有待排空的条目，不需要。）

### 前端单飞

`SearchViewModel` 的搜索请求收拢成 `performSearch → enqueueSearch → startSearch`：

- `searchInFlight` 标记一次在飞的引擎查询及其行转换；
- 忙时新请求只写进 `deferredSearch`（新的覆盖旧的，列表只需要最新答案）；
- 完成回调里先清标记再判 `searchGeneration`，然后 `drainDeferredSearch()` 回放；
  过期的那一条直接丢弃——当次输入自己的 debounce 随后会在空闲时重新入队。

## 测试

- `tests/test_wal_batch_fsync.h` 新增三条契约：
  - 追加 8 条后未排空时 `readAll()` 为空（把「批量写」这件事固定成断言，防止
    以后有人以为每条都已落盘）；`flushPending()` 后 8 条全部可读且首尾正确；
  - `sync()` 同样把缓冲条目送进文件；
  - 重复 `flushPending()` 是 no-op：`currentSize()` 不变、文件仍只有 1 条。
- `tests/test_fsevents_batch.h` Test 7：`batchMutate` / `removeByPathPrefix` /
  `batchRescanPrefix` 在 **不关闭 WAL** 的前提下条目立即可读
  （30 增 + 30 删 + 10 播种 + 10 陈旧删 + 5 新增 = 85 条），末条确认为最后一条新记录。
- Test 8：`hasActiveQuery()` 在 4 线程持续查询期间为真、全部结束后为假。轮询条件
  是「看到在飞 **且** 至少完成 4 轮」，因此断言不依赖单机上一次查询要跑多久。
- `./test_all --fast`：**12233 passed / 0 failed**，连跑三次结果一致；
  `make test-asan` 同一套断言全绿，无 sanitizer 报告。
- `swiftc -typecheck` 覆盖 `MacSoSo/App/*.swift` 通过（只保留既有的 Sendable 警告）。

## 效果与代价

- 索引独占锁里的磁盘写清零：一个批次一次 `flushPending()`，查询不再排在几百次
  `write()` 后面。
- 压缩不再和用户搜索撞在同一时间窗，第二内存峰（快照 + 640 MB 重写）被推到空闲
  时刻。
- 一万行结果最多同时存在一份，刷新风暴被折叠成一次回放。
- 代价：崩溃窗口内 ≤8 KB 的 WAL 尚未落盘（由启动重放 FSEvents 兜底）；搜索持续
  繁忙时压缩最多延后 120 s；`Table` 本身的渲染成本没有变化——它从来不是瓶颈。

## 相关条目

- [152](152-idle-memory-and-bulk-rescan.md)：批量重扫提速与常驻内存压缩
- [153](153-posting-index-varint-arena.md)：倒排链 varint 压缩（内存第一峰的直接解法，
  需要重新打包 Release 才在旧构建上生效）
