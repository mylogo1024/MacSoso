#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3b: Path-based Search Tests
// ═══════════════════════════════════════════════════════

static void runPathSearchTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3b: Path-based Search Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;

    std::vector<FileRecord> records;
    records.push_back({"main.cpp", "/usr/local/src", 1, 100, 1000});
    records.push_back({"config.json", "/etc/myapp", 1, 200, 2000});
    records.push_back({"readme.md", "/home/user/projects", 1, 300, 3000});
    records.push_back({"libfoo.dylib", "/usr/local/lib", 1, 400, 4000});
    records.push_back({"include", "/usr/local", 2, 0, 5000});
    engine.loadRecords(std::move(records));

    // Everything's default terms match the filename only, not the path.
    auto res = engine.query("local");
    check(res.size() == 0, "Filename search 'local': 0 matches (path is not searched)");

    // path: explicitly searches the parent folder path
    res = engine.query("path:local");
    check(res.size() == 3, "path:local: 3 matches (path contains 'local')");

    // A term containing a path separator matches the full path.
    // "/usr/local" is a prefix of the full paths below /usr/local.
    res = engine.query("/usr/local");
    check(res.size() == 3, "Path search '/usr/local': 3 matches (full path contains '/usr/local')");

    // Path + filename terms combine with AND
    res = engine.query("path:/etc config");
    check(res.size() == 1, "Path search 'path:/etc config': 1 match");
    check(engine.getRecord(res[0]).name == "config.json", "Path search 'path:/etc config': correct file");

    // A bare directory name is not matched against the path by default
    res = engine.query("projects");
    check(res.size() == 0, "Dir name search 'projects': 0 matches (filename only)");
    res = engine.query("path:projects");
    check(res.size() == 1, "path:projects: 1 match");
    if (!res.empty())
        check(engine.getRecord(res[0]).name == "readme.md", "path:projects: correct file");

    // Path glob: '**' crosses separators, '*' does not
    res = engine.query("**/lib/*");
    check(res.size() == 1, "Glob '**/lib/*': 1 match");
    if (!res.empty())
        check(engine.getRecord(res[0]).name == "libfoo.dylib", "Glob '**/lib/*': correct file");

    // Path search after addRecord (plain search)
    engine.addRecord({"newlib.a", "/usr/local/lib", 1, 500, 6000});
    // "newlib" (plain) finds the new file
    res = engine.query("newlib");
    check(res.size() == 1, "After addRecord: 'newlib' returns 1 match");

    // Path search after removeByPath
    engine.removeByPath("/usr/local/lib/libfoo.dylib");
    res = engine.query("libfoo");
    check(res.size() == 0, "After removeByPath: 'libfoo' returns 0 match");

    // Path search after updateByPath
    engine.updateByPath("/usr/local/lib/newlib.a", {"newlib_v2.a", "/opt/lib", 1, 600, 7000});
    res = engine.query("newlib_v2");
    check(res.size() == 1, "After updateByPath: 'newlib_v2' returns 1 match");

    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════
//  Part 3b-2: lowerPathPool_ / Dedup Path Matching Tests
// ═══════════════════════════════════════════════════════

static void runLowerPathPoolTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3b-2: lowerPathPool & Dedup Path Match\n";
    std::cout << "========================================\n\n";

    // --- 1. Case-insensitive path search via lowerPathPool_ ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"App.swift", "/Users/Dev/MyProject/Sources", 1, 100, 1000});
        records.push_back({"main.cpp", "/Volumes/DATA/CppProject/src", 1, 200, 2000});
        records.push_back({"readme.txt", "/home/User/Documents", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // path: searches the parent path, case-insensitively
        auto res = engine.query("path:myproject");
        check(res.size() == 1, "lowerPathPool: 'path:myproject' matches '/Users/Dev/MyProject/Sources'");
        check(engine.getRecord(res[0]).name == "App.swift", "lowerPathPool: correct file for 'path:myproject'");

        res = engine.query("path:CPPPROJECT");
        check(res.size() == 1, "lowerPathPool: 'path:CPPPROJECT' matches path with mixed case");

        res = engine.query("path:documents");
        check(res.size() == 1, "lowerPathPool: 'path:documents' matches '/home/User/Documents'");

        // Short keyword (2 chars) triggers linear scan path
        res = engine.query("path:de");
        check(res.size() >= 1, "lowerPathPool: 2-char search 'path:de' finds path match");
    }

    // --- 2. addRecord preserves lowerPathPool_ invariant ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"base.txt", "/root", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        // Add record with mixed-case path
        engine.addRecord({"NewFile.py", "/Users/Admin/PyProject", 1, 200, 2000});

        auto res = engine.query("path:pyproject");
        check(res.size() == 1, "addRecord lowerPathPool: 'path:pyproject' matches after addRecord");
        check(engine.getRecord(res[0]).name == "NewFile.py", "addRecord lowerPathPool: correct file");

        res = engine.query("path:admin");
        check(res.size() == 1, "addRecord lowerPathPool: 'path:admin' matches path '/Users/Admin/...'");
    }

    // --- 3. updateByPath preserves lowerPathPool_ invariant ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"old.txt", "/Data/OldDir", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        engine.updateByPath("/Data/OldDir/old.txt", {"new.txt", "/Data/NewDir/SubFolder", 1, 200, 2000});

        auto res = engine.query("path:olddir");
        check(res.size() == 0, "updateByPath lowerPathPool: old path no longer matches");

        res = engine.query("path:subfolder");
        check(res.size() == 1, "updateByPath lowerPathPool: new path matches");
        check(engine.getRecord(res[0]).name == "new.txt", "updateByPath lowerPathPool: correct updated file");
    }

    // --- 4. compaction preserves lowerPathPool_ and path search ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"a.txt", "/Path/Alpha", 1, 100, 1000});
        records.push_back({"b.txt", "/Path/Beta", 1, 200, 2000});
        records.push_back({"c.txt", "/Path/Gamma", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // Remove middle record to create tombstone
        engine.removeByPath("/Path/Beta/b.txt");
        check(engine.liveRecordCount() == 2, "compaction setup: 2 live records");

        // Compact
        auto mapping = engine.compactRecords();
        check(!mapping.empty(), "compaction occurred");
        check(engine.liveRecordCount() == 2, "compaction: still 2 live records");

        // Path search still works after compaction with case-insensitive match
        auto res = engine.query("path:alpha");
        check(res.size() == 1, "post-compaction lowerPathPool: 'path:alpha' matches");
        check(engine.getRecord(res[0]).name == "a.txt", "post-compaction: correct file for 'path:alpha'");

        res = engine.query("path:gamma");
        check(res.size() == 1, "post-compaction lowerPathPool: 'path:gamma' matches");

        res = engine.query("path:beta");
        check(res.size() == 0, "post-compaction: removed path 'beta' no longer matches");
    }

    // --- 5. Slash-boundary query with node-centric semantics ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // Path ends with "foo", name starts with "bar"
        records.push_back({"bar_file.txt", "/data/foo", 1, 100, 1000});
        records.push_back({"other.txt", "/data/baz", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // "foo/bar" → node-centric: name contains "bar", path contains "foo"
        auto res = engine.query("foo/bar");
        check(res.size() == 1, "slash-boundary: 'foo/bar' matches bar_file.txt (name has 'bar', path has 'foo')");
        check(engine.getRecord(res[0]).name == "bar_file.txt", "slash-boundary: correct file");

        // Another test: "settings/config"
        SearchEngine engine2;
        std::vector<FileRecord> records2;
        records2.push_back({"config.yml", "/app/Settings", 1, 100, 1000});
        records2.push_back({"data.json", "/app/Cache", 1, 200, 2000});
        engine2.loadRecords(std::move(records2));

        // "settings/config" → name contains "config", path contains "settings"
        res = engine2.query("settings/config");
        check(res.size() == 1, "slash-boundary: 'settings/config' matches config.yml");
    }

    // --- 6. resolveRecordPath returns original casing (not lowered) ---
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"Test.cpp", "/Users/Dev/MyProject", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        std::string path = engine.resolveRecordPath(0);
        check(path == "/Users/Dev/MyProject", "resolveRecordPath returns original casing, not lowered");
    }

    std::cout << "\n";
}
