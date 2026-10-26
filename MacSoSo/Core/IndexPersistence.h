#pragma once
#include "SearchEngine.h"
#include "ContentIndex.h"
#include "ContentIndexPersistence.h"
#include "IndexWAL.h"
#include "PagedIndexWriter.h"
#include "FlatIndexWriter.h"
#include "FileSystemWatcher.h"
#include <string>
#include <memory>
#include <mutex>
#include <dispatch/dispatch.h>

/// Orchestrates index persistence: paged base files + WAL + auto-compaction.
class IndexPersistence {
public:
    IndexPersistence(std::shared_ptr<SearchEngine> engine,
                     const std::string& basePath,
                     const std::string& walPath,
                     const std::string& pagesPath,
                     const std::string& ptablePath,
                     const std::string& v6Path);
    ~IndexPersistence();

    IndexPersistence(const IndexPersistence&) = delete;
    IndexPersistence& operator=(const IndexPersistence&) = delete;

    /// Load base index + replay WAL entries.
    /// Tries paged format first, falls back to v3 format, auto-migrates.
    /// Returns the lastEventId from the base file (0 if no file or v1 format).
    uint64_t load();

    /// Start logging mutations to WAL.
    void attachWAL();

    /// Minimum WAL entry count before flush proceeds (unless forced).
    static constexpr uint64_t kCompactThreshold = 100;

    /// Incremental flush: write only dirty pages, swap WAL.
    void flush(uint64_t lastEventId, bool force = false);
    void flush(const IndexMetadata& metadata, bool force = false);

    /// Full compaction: compactRecords() + fullRewrite() + remap ContentIndex.
    /// Called when tombstone ratio > kTombstoneCompactRatio.
    void fullCompact(const IndexMetadata& metadata);

    /// Legacy compact() — delegates to flush() for backward compatibility.
    void compact(uint64_t lastEventId, bool force = false) { flush(lastEventId, force); }
    void compact(const IndexMetadata& metadata, bool force = false) { flush(metadata, force); }

    /// Set the ContentIndex so compaction can propagate index remapping.
    void setContentIndex(std::shared_ptr<ContentIndex> ci) { contentIndex_ = std::move(ci); }

    /// Set the ContentIndexPersistence so fullCompact can flush content index after remap.
    void setContentIndexPersistence(std::shared_ptr<ContentIndexPersistence> cp) { contentPersistence_ = std::move(cp); }

    /// Start a GCD timer that flushes with adaptive interval.
    void startAutoCompaction(double intervalSec, std::shared_ptr<FileSystemWatcher> watcher);

    /// Stop auto-compaction and wait for in-flight work to finish.
    void stopAutoCompactionAndWait();

    const std::string& basePath() const { return basePath_; }
    const std::string& walPath() const { return walPath_; }

    // Adaptive interval constants
    static constexpr double kBaseIntervalSec = 300.0;
    static constexpr double kMinIntervalSec  = 30.0;
    static constexpr double kMaxIntervalSec  = 600.0;
    static constexpr size_t kWALSizeFlushThreshold = 2 * 1024 * 1024; // 2MB
    /// Below this WAL size a non-forced flush skips rewriting the whole base
    /// file: the WAL already durably records every incremental change and is
    /// replayed on the next load. This decouples cheap WAL growth from
    /// expensive full-base checkpoints (which can be hundreds of MB for a
    /// large index). Forced flushes (shutdown / manual sync) always fold.
    static constexpr size_t kWALFoldSizeThreshold = 16 * 1024 * 1024; // 16MB
    static constexpr double kTombstoneCompactRatio = 0.25;
    static constexpr double kDeadSpaceRewriteRatio = 0.5;
    /// When a tick finds a query in flight it re-arms after this delay instead
    /// of starting the heavy snapshot+rewrite on top of the search.
    static constexpr double kQueryBusyRetrySec = 20.0;
    /// ... and stops deferring after this many retries, so a busy client cannot
    /// starve durability or capacity reclamation forever.
    static constexpr int kMaxDeferredTicks = 6;

private:
    std::shared_ptr<SearchEngine> engine_;
    std::shared_ptr<ContentIndex> contentIndex_;
    std::shared_ptr<ContentIndexPersistence> contentPersistence_;
    std::shared_ptr<IndexWAL> wal_;
    std::unique_ptr<PagedIndexWriter> pagedWriter_;
    std::unique_ptr<FlatIndexWriter> flatWriter_;
    std::string basePath_;   // legacy v3 path (index.bin)
    std::string v6Path_;
    std::string walPath_;
    dispatch_source_t timer_ = nullptr;
    dispatch_queue_t  timerQueue_ = nullptr;
    double currentIntervalSec_ = 0;
    /// Consecutive ticks skipped because a query was in flight (timer queue only).
    int deferredTicks_ = 0;
    mutable std::mutex walMutex_;

    /// Compute the next auto-compaction interval based on dirty ratio and WAL size.
    double computeAdaptiveInterval() const;

    /// Stop the timer and synchronously wait for any in-flight callback to finish.
    void stopTimer();

    /// Change the fire interval. No-op if the change is < 1 second.
    void rescheduleTimer(double intervalSec);
};
