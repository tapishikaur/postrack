#include "postrack/fill_log.hpp"

#include "postrack/crc32c.hpp"
#include "postrack/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace postrack {
namespace {

constexpr std::size_t kHeader = 8;            // u32 length + u32 crc
constexpr std::size_t kBufferLimit = 1 << 16; // flush the write buffer at 64 KiB

[[noreturn]] void ioError(const std::string& what, const std::string& path) {
  throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

// A raw file handle with the few operations the log needs, on Windows and POSIX.
class File {
public:
  enum class Access { ReadWriteCreate, ReadOnly };
  File(const std::string& path, Access access) : path_(path) {
#ifdef _WIN32
    fd_ = access == Access::ReadOnly ? ::_open(path.c_str(), _O_RDONLY | _O_BINARY)
                                     : ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    fd_ = access == Access::ReadOnly ? ::open(path.c_str(), O_RDONLY) : ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
#endif
    if (fd_ < 0) ioError("can't open", path);
  }
  File(File&& other) noexcept : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)) {}
  File& operator=(File&& other) noexcept {
    std::swap(path_, other.path_);
    std::swap(fd_, other.fd_);
    return *this;
  }
  ~File() {
    if (fd_ >= 0) {
#ifdef _WIN32
      ::_close(fd_);
#else
      ::close(fd_);
#endif
    }
  }

  std::string readAll() {
    seek(0);
    std::string data;
    char chunk[1 << 16];
    while (true) {
#ifdef _WIN32
      const int n = ::_read(fd_, chunk, sizeof chunk);
#else
      const ssize_t n = ::read(fd_, chunk, sizeof chunk);
#endif
      if (n < 0) ioError("can't read", path_);
      if (n == 0) break;
      data.append(chunk, static_cast<std::size_t>(n));
    }
    return data;
  }
  void truncate(std::uint64_t size) {
#ifdef _WIN32
    if (::_chsize_s(fd_, static_cast<long long>(size)) != 0) ioError("can't truncate", path_);
#else
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) ioError("can't truncate", path_);
#endif
  }
  void seek(std::uint64_t offset) {
#ifdef _WIN32
    if (::_lseeki64(fd_, static_cast<long long>(offset), SEEK_SET) < 0) ioError("can't seek", path_);
#else
    if (::lseek(fd_, static_cast<off_t>(offset), SEEK_SET) < 0) ioError("can't seek", path_);
#endif
  }
  void write(const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
#ifdef _WIN32
      const int n = ::_write(fd_, data.data() + done, static_cast<unsigned>(data.size() - done));
#else
      const ssize_t n = ::write(fd_, data.data() + done, data.size() - done);
#endif
      if (n < 0) {
        if (errno == EINTR) continue;
        ioError("can't write", path_);
      }
      done += static_cast<std::size_t>(n);
    }
  }
  void forceToDisk() {
#ifdef _WIN32
    if (::_commit(fd_) != 0) ioError("can't flush", path_); // FlushFileBuffers
#else
    if (::fdatasync(fd_) != 0) ioError("can't flush", path_);
#endif
  }

private:
  std::string path_;
  int fd_ = -1;
};

// Walks the records in `data`. Returns how many bytes are good; the rest is a torn tail.
// Throws LogCorrupted for anything that isn't a torn tail.
std::uint64_t scan(const std::string& data, const std::string& path, const std::function<void(const Fill&)>& onRecord,
                   LogOpenStats& stats) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t size = data.size();
  std::size_t pos = 0;
  std::uint64_t expectedSeq = 1;

  const auto restIsZero = [&](std::size_t from) {
    return std::all_of(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), [](char c) { return c == 0; });
  };
  const auto corrupted = [&](const std::string& why) -> LogCorrupted {
    return LogCorrupted(path + ": " + why + " at byte " + std::to_string(pos) +
                        "; this is not a torn write from a crash, so the log needs checking by hand");
  };

  while (pos < size) {
    if (size - pos < kHeader) return pos; // incomplete header at the end: torn
    const std::uint32_t length = getU32(bytes + pos);
    const std::uint32_t crc = getU32(bytes + pos + 4);
    if (length == 0 || length > kMaxBody) {
      if (restIsZero(pos)) return pos; // zero-filled tail after a crash
      throw corrupted("record length " + std::to_string(length) + " out of range");
    }
    if (size - pos - kHeader < length) return pos; // incomplete body at the end: torn
    const auto body = std::span<const std::uint8_t>(bytes + pos + kHeader, length);
    const bool last = pos + kHeader + length == size;
    if (crc32c(body.data(), body.size()) != crc) {
      if (last) return pos; // the final record was only partly written
      throw corrupted("checksum mismatch");
    }
    Message message;
    try {
      message = decodeBody(body);
    } catch (const ProtocolError& e) {
      throw corrupted(std::string("record with a good checksum but bad contents (") + e.what() + ")");
    }
    const Fill* fill = std::get_if<Fill>(&message);
    if (!fill) throw corrupted("a record that isn't a fill");
    if (fill->seq != expectedSeq) {
      throw corrupted("seq " + std::to_string(fill->seq) + " where " + std::to_string(expectedSeq) + " was expected");
    }
    onRecord(*fill);
    ++expectedSeq;
    ++stats.records;
    stats.lastSeq = fill->seq;
    pos += kHeader + length;
  }
  return pos;
}

} // namespace

SyncMode parseSyncMode(const std::string& text) {
  if (text == "batch") return SyncMode::Batch;
  if (text == "every") return SyncMode::Every;
  if (text == "none") return SyncMode::None;
  throw std::invalid_argument("sync mode must be batch, every or none, not '" + text + "'");
}

struct FillLog::Impl {
  File file;
  SyncMode mode;
  std::string buffer;
  std::uint64_t lastSeq = 0;
  std::uint64_t syncs = 0;

  void flush() {
    if (buffer.empty()) return;
    file.write(buffer);
    buffer.clear();
  }
};

FillLog::FillLog(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FillLog::FillLog(FillLog&&) noexcept = default;
FillLog& FillLog::operator=(FillLog&&) noexcept = default;

FillLog::~FillLog() {
  if (!impl_) return;
  try {
    impl_->flush();
  } catch (...) {
    // Destructors must not throw; anything unwritten is simply not in the log, and the
    // upstream will resend it after a restart.
  }
}

FillLog FillLog::open(const std::string& path, SyncMode mode, const std::function<void(const Fill&)>& onRecord,
                      LogOpenStats* statsOut) {
  File file(path, File::Access::ReadWriteCreate);
  const std::string data = file.readAll();
  LogOpenStats stats;
  const std::uint64_t good = scan(data, path, onRecord, stats);
  if (good < data.size()) {
    stats.truncatedBytes = data.size() - good;
    file.truncate(good);
    file.forceToDisk();
  }
  file.seek(good);
  auto impl = std::make_unique<Impl>(Impl{std::move(file), mode, {}, stats.lastSeq, 0});
  if (statsOut) *statsOut = stats;
  return FillLog(std::move(impl));
}

void FillLog::append(const Fill& fill) {
  if (fill.seq != impl_->lastSeq + 1) {
    throw std::logic_error("fill log append out of order: seq " + std::to_string(fill.seq) + " after " +
                           std::to_string(impl_->lastSeq));
  }
  std::string body;
  encodeBody(fill, body);
  std::string& out = impl_->buffer;
  putU32(out, static_cast<std::uint32_t>(body.size()));
  putU32(out, crc32c(body.data(), body.size()));
  out += body;
  impl_->lastSeq = fill.seq;
  if (impl_->mode == SyncMode::Every) {
    sync();
  } else if (out.size() >= kBufferLimit) {
    impl_->flush();
  }
}

void FillLog::sync() {
  impl_->flush();
  if (impl_->mode != SyncMode::None) {
    impl_->file.forceToDisk();
    ++impl_->syncs;
  }
}

std::uint64_t FillLog::lastSeq() const { return impl_->lastSeq; }
std::uint64_t FillLog::syncs() const { return impl_->syncs; }

LogOpenStats readFillLog(const std::string& path, const std::function<void(const Fill&)>& onRecord) {
  File file(path, File::Access::ReadOnly);
  const std::string data = file.readAll();
  LogOpenStats stats;
  const std::uint64_t good = scan(data, path, onRecord, stats);
  stats.truncatedBytes = data.size() - good;
  return stats;
}

} // namespace postrack
