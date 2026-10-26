// Tests for compaction threshold: skip compaction when WAL entry count is below threshold
#pragma once

static void testCompactSkipsBelowThreshold() {
    std::cout << "\n--- Compact skips below threshold ---\n";

    std::string tmpDir = "/tmp/test_compact_threshold_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    std::string basePath = tmpDir + "/index.bin";
    std::string walPath  = tmpDir + "/index.wal";

    auto engine = std::make_shared<SearchEngine>();
    IndexPersistence persistence(engine, basePath, walPath, pagesPathFor(basePath), ptablePathFor(basePath), basePath + ".v6");
    persistence.attachWAL();

    // Add a few records (below kCompactThreshold = 100)
    for (int i = 0; i < 5; i++) {
        FileRecord rec;
        rec.name = "file" + std::to_string(i) + ".txt";
        rec.path = tmpDir;
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);
        engine->addRecord(std::move(rec));
    }

    // Compact without force — should skip because only 5 entries (< 100)
    persistence.compact(1);
    check(!fs::exists(ptablePathFor(basePath)), "ptable should NOT be written when below threshold");

    fs::remove_all(tmpDir);
}

static void testCompactProceedsAtThreshold() {
    std::cout << "\n--- Compact at threshold keeps incremental WAL (no base rewrite) ---\n";

    std::string tmpDir = "/tmp/test_compact_threshold2_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    std::string basePath = tmpDir + "/index.bin";
    std::string walPath  = tmpDir + "/index.wal";

    auto engine = std::make_shared<SearchEngine>();
    IndexPersistence persistence(engine, basePath, walPath, pagesPathFor(basePath), ptablePathFor(basePath), basePath + ".v6");
    persistence.attachWAL();

    // Add exactly kCompactThreshold records
    for (uint64_t i = 0; i < IndexPersistence::kCompactThreshold; i++) {
        FileRecord rec;
        rec.name = "file" + std::to_string(i) + ".txt";
        rec.path = tmpDir;
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);
        engine->addRecord(std::move(rec));
    }

    // A non-forced compact at threshold with a small WAL must NOT rewrite the
    // entire base: the WAL already durably records every increment and is
    // replayed on next load. Only force=true or a WAL that exceeds
    // kWALFoldSizeThreshold folds the base (see testCompactForceIgnoresThreshold).
    persistence.compact(1);
    check(!fs::exists(basePath + ".v6"),
          "v6 base should NOT be rewritten for a small incremental WAL (incremental-only)");

    // The increments must NOT be lost: the WAL still holds every entry so a
    // later load can replay them on top of the (unchanged) base.
    auto walEntries = IndexWAL::readAll(walPath);
    check(walEntries.size() == IndexPersistence::kCompactThreshold,
          "increments preserved in WAL after non-forced compact");

    fs::remove_all(tmpDir);
}

static void testCompactForceIgnoresThreshold() {
    std::cout << "\n--- Compact force=true ignores threshold ---\n";

    std::string tmpDir = "/tmp/test_compact_threshold3_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    std::string basePath = tmpDir + "/index.bin";
    std::string walPath  = tmpDir + "/index.wal";

    auto engine = std::make_shared<SearchEngine>();
    IndexPersistence persistence(engine, basePath, walPath, pagesPathFor(basePath), ptablePathFor(basePath), basePath + ".v6");
    persistence.attachWAL();

    // Add just 1 record (well below threshold)
    FileRecord rec;
    rec.name = "single.txt";
    rec.path = tmpDir;
    rec.type = 1;
    rec.size = 42;
    rec.modTime = time(nullptr);
    engine->addRecord(std::move(rec));

    // Compact with force=true — should proceed regardless of entry count
    persistence.compact(1, /*force=*/true);
    check(fs::exists(basePath + ".v6"), "v6 index SHOULD be written when force=true");

    fs::remove_all(tmpDir);
}

static void runCompactThresholdTests() {
    testCompactSkipsBelowThreshold();
    testCompactProceedsAtThreshold();
    testCompactForceIgnoresThreshold();
}
