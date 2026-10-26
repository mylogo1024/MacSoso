#pragma once
// ═══════════════════════════════════════════════════════
//  Part 79: Query Result Cache + Incremental Persistence
//
//  (a) Unit tests for QueryResultCache: miss → insert → hit,
//      LRU eviction, clear, and large-set rejection.
//  (b) Integration tests proving SearchEngine serves a repeated
//      query from cache (timing.fromCache == true) and that any
//      index mutation invalidates the cache so stale answers are
//      never returned.
//  (c) Incremental-persistence contract: a non-forced flush with
//      a small WAL does NOT rewrite the base (verified in Part 31),
//      and a forced flush still folds everything into the base.
// ═══════════════════════════════════════════════════════

#include "../MacSoSo/Core/QueryCache.h"
#include "../MacSoSo/Core/SearchEngine.h"
#include "../MacSoSo/Core/IndexPersistence.h"
#include "test_helpers.h"
#include <filesystem>
#include <vector>
#include <string>

// ── (a) QueryResultCache unit tests ──
static void queryCacheUnitTests() {
    std::cout << "\n--- QueryResultCache unit ---\n";

    // Miss on empty.
    {
        QueryResultCache c;
        std::vector<uint32_t> out;
        check(!c.lookup("k", out), "empty cache misses");
        check(c.size() == 0, "empty cache size 0");
    }

    // Insert then hit returns the same payload.
    {
        QueryResultCache c;
        std::vector<uint32_t> v{1, 2, 3, 7};
        c.insert("key", v);
        std::vector<uint32_t> out;
        check(c.lookup("key", out), "inserted key hits");
        check(out == v, "hit payload matches inserted value");
        check(c.size() == 1, "cache size 1 after insert");
    }

    // Overwrite replaces the payload.
    {
        QueryResultCache c;
        c.insert("key", {1, 2});
        c.insert("key", {9});
        std::vector<uint32_t> out;
        check(c.lookup("key", out), "overwritten key still hits");
        check(out == std::vector<uint32_t>{9}, "overwrite replaces payload");
        check(c.size() == 1, "size unchanged after overwrite");
    }

    // LRU eviction respects capacity and recency.
    {
        QueryResultCache c(2);
        c.insert("a", {1});
        c.insert("b", {2});
        check(c.size() == 2, "cache at capacity");
        // Touch "a" so "b" becomes least-recently-used.
        std::vector<uint32_t> out;
        check(c.lookup("a", out), "a hits before eviction");
        c.insert("c", {3});   // evicts b
        check(c.size() == 2, "still at capacity after eviction");
        check(c.lookup("a", out), "a retained (recently used)");
        check(!c.lookup("b", out), "b evicted (least recently used)");
        check(c.lookup("c", out), "c retained (just inserted)");
    }

    // clear() drops everything.
    {
        QueryResultCache c;
        c.insert("a", {1});
        c.insert("b", {2});
        c.clear();
        std::vector<uint32_t> out;
        check(c.size() == 0, "clear empties cache");
        check(!c.lookup("a", out), "clear invalidates entries");
    }

    // Oversized entries are rejected so the cache can't balloon.
    {
        QueryResultCache c(16, /*maxEntryBytes=*/1024);
        std::vector<uint32_t> big(2048, 5);   // 8 KiB > 1 KiB cap
        c.insert("big", big);
        check(c.size() == 0, "oversized entry rejected");
        std::vector<uint32_t> out;
        check(!c.lookup("big", out), "oversized entry not cached");
    }
}

// ── (b) SearchEngine cache integration ──
static void searchEngineCacheIntegration() {
    std::cout << "\n--- SearchEngine result-cache integration ---\n";

    SearchEngine engine;
    std::vector<FileRecord> recs;
    for (int i = 0; i < 50; i++) {
        FileRecord r;
        r.name = "alpha_" + std::to_string(i) + ".txt";
        r.path = "/data/cache_test";
        r.type = 1;
        r.size = static_cast<uint64_t>(100 + i);
        r.modTime = static_cast<time_t>(1600000000 + i);
        recs.push_back(std::move(r));
    }
    recs.push_back({"beta.txt", "/data/cache_test", 1, 5, 1600000000});
    engine.loadRecords(std::move(recs));

    QueryTimingInfo t1;
    auto r1 = engine.query("alpha", 100, true, t1);
    check(r1.size() == 50, "first query returns all alpha records");
    check(!t1.fromCache, "first query is NOT from cache");
    check(engine.queryCacheSize() >= 1, "cache populated after first query");

    // Same query, same flags → cache hit, identical payload.
    QueryTimingInfo t2;
    auto r2 = engine.query("alpha", 100, true, t2);
    check(t2.fromCache, "second identical query is served from cache");
    check(r1 == r2, "cached payload equals fresh payload");

    // Different query → miss.
    QueryTimingInfo t3;
    auto r3 = engine.query("beta", 100, true, t3);
    check(r3.size() == 1, "beta query finds its record");
    check(!t3.fromCache, "distinct query is a cache miss");

    // A different maxResults produces a different key → miss (correctness:
    // the returned set must still be right).
    QueryTimingInfo t4;
    auto r4 = engine.query("alpha", 10, true, t4);
    check(!t4.fromCache, "different maxResults is a different key (miss)");
    check(r4.size() == 10, "capped query returns exactly the cap");

    // ── Invalidation: addRecord changes the answer set ──
    engine.addRecord({"alpha_new.txt", "/data/cache_test", 1, 7, 1600000000});
    QueryTimingInfo t5;
    auto r5 = engine.query("alpha", 100, true, t5);
    check(!t5.fromCache, "addRecord invalidates cache (miss after mutation)");
    check(r5.size() == 51, "query sees the newly added record");

    // ── Invalidation: removeByPath changes the answer set ──
    engine.removeByPath("/data/cache_test/alpha_new.txt");
    QueryTimingInfo t6;
    auto r6 = engine.query("alpha", 100, true, t6);
    check(!t6.fromCache, "removeByPath invalidates cache (miss after mutation)");
    check(r6.size() == 50, "query no longer sees the removed record");

    // After invalidation, a re-run is a hit again with the correct set.
    QueryTimingInfo t7;
    auto r7 = engine.query("alpha", 100, true, t7);
    check(t7.fromCache, "re-run after invalidation is a hit");
    check(r7 == r6, "post-invalidation cached payload is correct");
}

// ── (c) Incremental persistence: force still folds ──
static void incrementalPersistenceForcedFold() {
    std::cout << "\n--- Incremental persistence: forced flush folds base ---\n";

    std::string tmpDir = "/tmp/test_query_cache_fold_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    std::string basePath = tmpDir + "/index.bin";
    std::string walPath  = tmpDir + "/index.wal";

    auto engine = std::make_shared<SearchEngine>();
    IndexPersistence persistence(engine, basePath, walPath,
                                 pagesPathFor(basePath), ptablePathFor(basePath),
                                 basePath + ".v6");
    persistence.attachWAL();

    for (int i = 0; i < 5; i++) {
        FileRecord rec;
        rec.name = "foldfile_" + std::to_string(i) + ".txt";
        rec.path = tmpDir;
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);
        engine->addRecord(std::move(rec));
    }

    // Non-forced flush with a tiny WAL → incremental only, no base rewrite.
    persistence.flush(1, /*force=*/false);
    check(!fs::exists(basePath + ".v6"),
          "non-forced flush with small WAL keeps base unwritten (incremental only)");

    // Forced flush → folds everything into the v6 base.
    persistence.flush(1, /*force=*/true);
    check(fs::exists(basePath + ".v6"),
          "forced flush writes the v6 base");

    fs::remove_all(tmpDir);
}

static void runQueryCacheTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 79: Query Cache & Incremental Persistence\n";
    std::cout << "========================================\n";
    queryCacheUnitTests();
    searchEngineCacheIntegration();
    incrementalPersistenceForcedFold();
}
