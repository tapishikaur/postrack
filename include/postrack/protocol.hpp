#pragma once
// The wire protocol (docs/protocol.md): length-prefixed little-endian binary frames.

#include "postrack/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace postrack {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::uint32_t kMaxBody = 4096;
inline constexpr std::size_t kMaxString = 64;

enum class MsgType : std::uint8_t { Resume = 1, Fill = 2, Mark = 3, Heartbeat = 4, End = 5 };

struct ResumeMsg {
  std::uint64_t lastSeq = 0;
  bool operator==(const ResumeMsg&) const = default;
};
struct MarkMsg {
  std::string symbol;
  PriceRaw price = 0;
  std::int64_t tsNs = 0;
  bool operator==(const MarkMsg&) const = default;
};
struct HeartbeatMsg {
  bool operator==(const HeartbeatMsg&) const = default;
};
struct EndMsg {
  std::uint64_t lastSeq = 0;
  bool operator==(const EndMsg&) const = default;
};

using Message = std::variant<ResumeMsg, Fill, MarkMsg, HeartbeatMsg, EndMsg>;

class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Appends the body (no length prefix) of a message. Throws ProtocolError for a message
// that breaks the field rules (empty or long strings, bad side, qty or price < 1).
void encodeBody(const Message& message, std::string& out);
// The whole frame: [u32 length][body].
std::string encodeFrame(const Message& message);
// Decodes exactly one body. Throws ProtocolError on anything malformed.
Message decodeBody(std::span<const std::uint8_t> body);

// Collects bytes from a stream and hands back whole messages.
class FrameDecoder {
public:
  void feed(const char* data, std::size_t size);
  void feed(std::string_view data) { feed(data.data(), data.size()); }
  // The next complete message, or nullopt until more bytes arrive. Throws ProtocolError on
  // a bad frame; after that the stream is unusable and the connection should be dropped.
  std::optional<Message> next();
  std::size_t buffered() const { return buffer_.size() - start_; }

private:
  std::string buffer_;
  std::size_t start_ = 0;
};

// Little-endian helpers, shared with the fill log.
void putU32(std::string& out, std::uint32_t v);
std::uint32_t getU32(const std::uint8_t* p);

} // namespace postrack
