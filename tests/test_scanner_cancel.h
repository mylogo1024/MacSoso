#pragma once
// Part 13: DirectoryScanner cancel + reusability

#include <sys/mount.h>

static void runScannerCancelTest() {
    std::cout << "═══ Part 13: DirectoryScanner Cancel ═══\n\n";

    // Volume-selection policy: local user volumes under /Volumes are indexed;
    // system firmlinks, network shares and autofs mounts are not.
    check(DirectoryScanner::isIndexableVolume("/Volumes/mac", "apfs", MNT_LOCAL),
          "C13: external APFS volume under /Volumes is indexable");
    check(DirectoryScanner::isIndexableVolume("/Volumes/USB", "exfat", MNT_LOCAL),
          "C13: exFAT USB volume is indexable");
    check(DirectoryScanner::isIndexableVolume("/Volumes/DOS", "msdos", MNT_LOCAL),
          "C13: FAT volume is indexable");
    check(!DirectoryScanner::isIndexableVolume("/System/Volumes/Data", "apfs", MNT_LOCAL),
          "C13: system firmlink is not indexable");
    check(!DirectoryScanner::isIndexableVolume("/Volumes/share", "smbfs", MNT_LOCAL),
          "C13: unsupported filesystem is not indexable");
    check(!DirectoryScanner::isIndexableVolume("/Volumes/nfs", "nfs", MNT_LOCAL),
          "C13: NFS mount is not indexable");
    check(!DirectoryScanner::isIndexableVolume("/Volumes/Time Machine", "apfs",
                                               MNT_LOCAL | MNT_DONTBROWSE),
          "C13: hidden (MNT_DONTBROWSE) volume is not indexed");
    check(!DirectoryScanner::isIndexableVolume("/Volumes/remote", "apfs", 0),
          "C13: non-local mount is not indexed");
    check(!DirectoryScanner::isIndexableVolume("/", "apfs", MNT_LOCAL),
          "C13: root mount is not an extra volume");
    check(!DirectoryScanner::isIndexableVolume("/Volumes/bad", nullptr, MNT_LOCAL),
          "C13: missing filesystem type is rejected");

    // Create a temp dir with enough structure to keep scanning
    std::string tmpDir = "/tmp/macsoso_cancel_test_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    for (int i = 0; i < 20; i++) {
        fs::create_directories(tmpDir + "/dir" + std::to_string(i));
        for (int j = 0; j < 5; j++) {
            std::ofstream(tmpDir + "/dir" + std::to_string(i) + "/file" + std::to_string(j) + ".txt") << "data";
        }
    }

    // Test 1: cancel() before scan — scan should return quickly
    {
        DirectoryScanner scanner;
        scanner.cancel();
        auto start = std::chrono::steady_clock::now();
        scanner.scan(tmpDir);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        auto results = scanner.takeResults();
        // scan() resets cancelled_ at the start, so it should complete normally
        check(results.size() > 0, "C13: scan() resets cancelled_ flag and runs normally");
    }

    // Test 2: cancel() from another thread during scan
    {
        DirectoryScanner scanner;
        std::atomic<bool> scanDone{false};

        std::thread scanThread([&] {
            scanner.scan("/usr");
            scanDone.store(true);
        });

        // Let it run a bit then cancel
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scanner.cancel();

        auto start = std::chrono::steady_clock::now();
        scanThread.join();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        check(scanDone.load(), "C13: scan() completed after cancel");
        check(elapsed < 5000, "C13: scan() returned within 5s after cancel");
        check(scanner.isCancelled(), "C13: isCancelled() returns true after cancel");
    }

    // Test 3: scanner is reusable after cancel
    {
        DirectoryScanner scanner;
        scanner.cancel();
        scanner.scan(tmpDir); // resets cancelled_, should work normally
        auto results = scanner.takeResults();
        check(results.size() > 0, "C13: scanner reusable after cancel (results found)");
        check(!scanner.isCancelled(), "C13: cancelled_ is reset on new scan()");
    }

    fs::remove_all(tmpDir);
    std::cout << "\n";
}
