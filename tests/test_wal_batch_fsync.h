#pragma once
// Part 10: WAL Batch Fsync Benchmark

static void runWalBatchFsyncBenchmark() {
    std::cout << "========================================\n";
    std::cout << "  Part 10: WAL Batch Fsync\n";
    std::cout << "========================================\n\n";

    std::string tmpDir = "/tmp/macsoso_wal_bench_" + std::to_string(getpid());
    fs::create_directories(tmpDir);

    const int numEntries = 200;

    // --- Benchmark IndexWAL with batch fsync (default: every 64 entries) ---
    {
        std::string walPath = tmpDir + "/bench_batched.wal";
        IndexWAL wal;
        wal.open(walPath);

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < numEntries; i++) {
            FileRecord rec;
            rec.name = "file_" + std::to_string(i) + ".txt";
            rec.path = "/test/path";
            rec.type = 1;
            rec.size = 1000 + i;
            rec.modTime = time(nullptr);
            wal.append(WALOp::Add, rec.path + "/" + rec.name, rec);
        }
        wal.close();
        auto t1 = std::chrono::steady_clock::now();
        double batchedTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "  " << numEntries << " WAL appends (batch fsync every 64): "
                  << std::fixed << std::setprecision(2) << batchedTime << "ms ("
                  << batchedTime / numEntries << "ms/entry)\n";

        // Verify all entries are readable
        auto entries = IndexWAL::readAll(walPath);
        check(entries.size() == numEntries, "WAL-Bench: all batched entries readable");

        // Verify data integrity
        check(entries[0].record.name == "file_0.txt", "WAL-Bench: first entry name correct");
        check(entries[numEntries - 1].record.name == "file_" + std::to_string(numEntries - 1) + ".txt",
              "WAL-Bench: last entry name correct");
        check(entries[0].op == WALOp::Add, "WAL-Bench: entry op correct");
    }

    // --- Benchmark ContentIndexWAL with batch fsync ---
    {
        std::string walPath = tmpDir + "/bench_content_batched.wal";
        ContentIndexWAL wal;
        wal.open(walPath);

        std::vector<Trigram> trigrams = {
            ContentIndex::makeTrigram('a', 'b', 'c'),
            ContentIndex::makeTrigram('d', 'e', 'f'),
            ContentIndex::makeTrigram('g', 'h', 'i'),
        };

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < numEntries; i++) {
            wal.appendAdd(static_cast<uint32_t>(i), 12345 + i, trigrams);
        }
        wal.close();
        auto t1 = std::chrono::steady_clock::now();
        double contentTime = std::chrono::duration<double>(t1 - t0).count() * 1000;

        std::cout << "  " << numEntries << " ContentWAL appends (batch fsync): "
                  << std::fixed << std::setprecision(2) << contentTime << "ms ("
                  << contentTime / numEntries << "ms/entry)\n";

        auto entries = ContentIndexWAL::readAll(walPath);
        check(entries.size() == numEntries, "WAL-Bench: all content WAL entries readable");
        check(entries[0].fileIndex == 0, "WAL-Bench: content entry fileIndex correct");
        check(entries[0].trigrams.size() == 3, "WAL-Bench: content entry trigram count correct");
    }

    // --- Test explicit sync() method ---
    {
        std::string walPath = tmpDir + "/bench_sync.wal";
        IndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0); // disable auto-fsync

        for (int i = 0; i < 10; i++) {
            FileRecord rec;
            rec.name = "sync_test_" + std::to_string(i) + ".txt";
            rec.path = "/test";
            rec.type = 1;
            wal.append(WALOp::Add, rec.path + "/" + rec.name, rec);
        }
        wal.sync(); // explicit fsync
        wal.close();

        auto entries = IndexWAL::readAll(walPath);
        check(entries.size() == 10, "WAL-Bench: explicit sync() entries readable");
    }

    // --- Test setSyncInterval ---
    {
        std::string walPath = tmpDir + "/bench_interval.wal";
        IndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(5); // fsync every 5 entries

        for (int i = 0; i < 20; i++) {
            FileRecord rec;
            rec.name = "interval_" + std::to_string(i) + ".txt";
            rec.path = "/test";
            rec.type = 1;
            wal.append(WALOp::Add, rec.path + "/" + rec.name, rec);
        }
        wal.close();

        auto entries = IndexWAL::readAll(walPath);
        check(entries.size() == 20, "WAL-Bench: sync interval entries all readable");
    }

    // --- Test mixed Add/Remove/Update operations ---
    {
        std::string walPath = tmpDir + "/bench_mixed.wal";
        IndexWAL wal;
        wal.open(walPath);

        FileRecord rec;
        rec.name = "mixed.txt";
        rec.path = "/test";
        rec.type = 1;
        rec.size = 100;
        rec.modTime = time(nullptr);

        wal.append(WALOp::Add, "/test/mixed.txt", rec);
        rec.size = 200;
        wal.append(WALOp::Update, "/test/mixed.txt", rec);
        wal.append(WALOp::Remove, "/test/mixed.txt");
        wal.close();

        auto entries = IndexWAL::readAll(walPath);
        check(entries.size() == 3, "WAL-Bench: mixed ops all readable");
        check(entries[0].op == WALOp::Add, "WAL-Bench: mixed op[0] is Add");
        check(entries[1].op == WALOp::Update, "WAL-Bench: mixed op[1] is Update");
        check(entries[2].op == WALOp::Remove, "WAL-Bench: mixed op[2] is Remove");
        check(entries[1].record.size == 200, "WAL-Bench: updated size correct");
    }

    // --- Contract: appends stay in the stdio buffer until drained ---
    // The batched write path is what keeps a write() syscall off the index
    // lock; these assertions pin down what the caller therefore owes the WAL:
    // a batch is only durable once flushPending(), sync() or close() ran.
    {
        std::string walPath = tmpDir + "/bench_buffered.wal";
        IndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0); // never auto-fsync

        for (int i = 0; i < 8; i++) {
            FileRecord rec;
            rec.name = "buffered_" + std::to_string(i) + ".txt";
            rec.path = "/test";
            rec.type = 1;
            wal.append(WALOp::Add, rec.path + "/" + rec.name, rec);
        }

        auto before = IndexWAL::readAll(walPath);
        check(before.empty(), "WAL-Bench: buffered entries invisible before flush");

        wal.flushPending();
        auto after = IndexWAL::readAll(walPath);
        check(after.size() == 8, "WAL-Bench: flushPending() makes buffered entries readable");
        check(after[0].record.name == "buffered_0.txt", "WAL-Bench: flushed first entry correct");
        check(after[7].record.name == "buffered_7.txt", "WAL-Bench: flushed last entry correct");

        wal.close();
    }

    // --- Contract: sync() also drains the buffer (it used to fsync stale bytes) ---
    {
        std::string walPath = tmpDir + "/bench_sync_drains.wal";
        IndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0);

        for (int i = 0; i < 5; i++) {
            FileRecord rec;
            rec.name = "syncdrain_" + std::to_string(i) + ".txt";
            rec.path = "/test";
            rec.type = 1;
            wal.append(WALOp::Add, rec.path + "/" + rec.name, rec);
        }

        wal.sync();
        auto entries = IndexWAL::readAll(walPath);
        check(entries.size() == 5, "WAL-Bench: sync() pushes buffered entries to the file");

        wal.close();
    }

    // --- Contract: flushPending() is cheap when nothing is pending ---
    {
        std::string walPath = tmpDir + "/bench_flush_noop.wal";
        IndexWAL wal;
        wal.open(walPath);
        wal.setSyncInterval(0);

        FileRecord rec;
        rec.name = "noop.txt";
        rec.path = "/test";
        rec.type = 1;
        wal.append(WALOp::Add, "/test/noop.txt", rec);
        wal.flushPending();
        size_t sizeAfterFlush = wal.currentSize();
        wal.flushPending(); // must not rewrite or grow anything
        check(wal.currentSize() == sizeAfterFlush, "WAL-Bench: redundant flushPending() is a no-op");
        check(IndexWAL::readAll(walPath).size() == 1, "WAL-Bench: no-op flush keeps one entry");

        wal.close();
    }

    fs::remove_all(tmpDir);
    std::cout << "\n";
}
