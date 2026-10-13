#pragma once
// The write-ahead fill log (docs/protocol.md, "Fill log"): every fill is appended here
// before it changes any position, so a crash can always be recovered by replaying it.

#include "postrack/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace postrack {

// When appended records are forced to disk.
//   Batch: on sync(), which the service calls once per batch read from the socket (default)
//   Every: after every record (safest, slowest)
//   None:  never forced; the OS writes them when it likes (fastest, for comparison only)
enum class SyncMode { Batch, Every, None };
SyncMode parseSyncMode(const std::string& text); // "batch" | "every" | "none"; throws on anything else

// The log is damaged somewhere other than its last record: not a crash pattern, so refuse.
class LogCorrupted : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct LogOpenStats {
  std::uint64_t records = 0;        // good records found
  std::uint64_t lastSeq = 0;        // seq of the last good record (0 if none)
  std::uint64_t truncatedBytes = 0; // torn tail cut off
};

class FillLog {
public:
  // Opens or creates the log, checks every record and calls onRecord for each one in order.
  // A torn last record is cut off. Throws LogCorrupted for damage before the end or for seq
  // numbers that aren't 1, 2, 3, ...; std::runtime_error for I/O errors.
  static FillLog open(const std::string& path, SyncMode mode, const std::function<void(const Fill&)>& onRecord,
                      LogOpenStats* stats = nullptr);

  FillLog(FillLog&&) noexcept;
  FillLog& operator=(FillLog&&) noexcept;
  ~FillLog(); // writes anything buffered (without forcing it to disk)

  // Appends the next fill; its seq must be lastSeq() + 1 (std::logic_error otherwise).
  // With SyncMode::Every it's on disk when this returns; otherwise call sync().
  void append(const Fill& fill);
  // Writes buffered records and, unless SyncMode::None, forces them to disk.
  void sync();

  std::uint64_t lastSeq() const;
  std::uint64_t syncs() const; // how many times the disk was forced (for benchmarks)

private:
  struct Impl;
  explicit FillLog(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Reads a log or a simulator dump without changing it; same checks as FillLog::open except
// that nothing is truncated (a torn tail is reported in stats). For tools and tests.
LogOpenStats readFillLog(const std::string& path, const std::function<void(const Fill&)>& onRecord);

} // namespace postrack
