#include "postrack/protocol.hpp"

#include <cstring>

namespace postrack {
namespace {

void putU8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
void putU64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putI64(std::string& out, std::int64_t v) { putU64(out, static_cast<std::uint64_t>(v)); }
void putStr(std::string& out, const std::string& s, const char* what) {
  if (s.empty() || s.size() > kMaxString) {
    throw ProtocolError(std::string(what) + " must be 1 to 64 bytes");
  }
  putU8(out, static_cast<std::uint8_t>(s.size()));
  out += s;
}
void header(std::string& out, MsgType type) {
  putU8(out, static_cast<std::uint8_t>(type));
  putU8(out, kProtocolVersion);
}

// Reads fields in order and fails cleanly on a short body.
class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> body) : body_(body) {}
  std::uint8_t u8() {
    need(1);
    return body_[pos_++];
  }
  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(body_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
  }
  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
  std::string str(const char* what) {
    const std::uint8_t n = u8();
    if (n == 0 || n > kMaxString) throw ProtocolError(std::string(what) + " must be 1 to 64 bytes");
    need(n);
    std::string s(reinterpret_cast<const char*>(body_.data() + pos_), n);
    pos_ += n;
    return s;
  }
  void done() const {
    if (pos_ != body_.size()) throw ProtocolError("bytes left over after the last field");
  }

private:
  void need(std::size_t n) const {
    if (body_.size() - pos_ < n) throw ProtocolError("body ends in the middle of a field");
  }
  std::span<const std::uint8_t> body_;
  std::size_t pos_ = 0;
};

void checkFill(const Fill& f) {
  if (f.seq == 0) throw ProtocolError("fill seq must be at least 1");
  if (f.side != Side::Buy && f.side != Side::Sell) throw ProtocolError("fill side must be 1 or 2");
  if (f.qty < 1) throw ProtocolError("fill qty must be at least 1");
  if (f.price < 1) throw ProtocolError("fill price must be at least 1");
}

} // namespace

void putU32(std::string& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::uint32_t getU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

void encodeBody(const Message& message, std::string& out) {
  if (const auto* m = std::get_if<ResumeMsg>(&message)) {
    header(out, MsgType::Resume);
    putU64(out, m->lastSeq);
  } else if (const auto* f = std::get_if<Fill>(&message)) {
    checkFill(*f);
    header(out, MsgType::Fill);
    putU64(out, f->seq);
    putU64(out, f->execId);
    putI64(out, f->tsNs);
    putStr(out, f->strategy, "strategy");
    putStr(out, f->symbol, "symbol");
    putU8(out, static_cast<std::uint8_t>(f->side));
    putI64(out, f->qty);
    putI64(out, f->price);
  } else if (const auto* k = std::get_if<MarkMsg>(&message)) {
    if (k->price < 1) throw ProtocolError("mark price must be at least 1");
    header(out, MsgType::Mark);
    putStr(out, k->symbol, "symbol");
    putI64(out, k->price);
    putI64(out, k->tsNs);
  } else if (std::holds_alternative<HeartbeatMsg>(message)) {
    header(out, MsgType::Heartbeat);
  } else if (const auto* e = std::get_if<EndMsg>(&message)) {
    header(out, MsgType::End);
    putU64(out, e->lastSeq);
  }
}

std::string encodeFrame(const Message& message) {
  std::string body;
  encodeBody(message, body);
  std::string frame;
  frame.reserve(4 + body.size());
  putU32(frame, static_cast<std::uint32_t>(body.size()));
  frame += body;
  return frame;
}

Message decodeBody(std::span<const std::uint8_t> body) {
  Reader r(body);
  const std::uint8_t type = r.u8();
  const std::uint8_t version = r.u8();
  if (version != kProtocolVersion) throw ProtocolError("unsupported protocol version " + std::to_string(version));
  switch (static_cast<MsgType>(type)) {
  case MsgType::Resume: {
    ResumeMsg m{r.u64()};
    r.done();
    return m;
  }
  case MsgType::Fill: {
    Fill f;
    f.seq = r.u64();
    f.execId = r.u64();
    f.tsNs = r.i64();
    f.strategy = r.str("strategy");
    f.symbol = r.str("symbol");
    f.side = static_cast<Side>(r.u8());
    f.qty = r.i64();
    f.price = r.i64();
    r.done();
    checkFill(f);
    return f;
  }
  case MsgType::Mark: {
    MarkMsg m;
    m.symbol = r.str("symbol");
    m.price = r.i64();
    m.tsNs = r.i64();
    r.done();
    if (m.price < 1) throw ProtocolError("mark price must be at least 1");
    return m;
  }
  case MsgType::Heartbeat:
    r.done();
    return HeartbeatMsg{};
  case MsgType::End: {
    EndMsg m{r.u64()};
    r.done();
    return m;
  }
  }
  throw ProtocolError("unknown message type " + std::to_string(type));
}

void FrameDecoder::feed(const char* data, std::size_t size) {
  // Drop consumed bytes now and then, so the buffer doesn't grow forever.
  if (start_ > 0 && start_ >= buffer_.size() / 2) {
    buffer_.erase(0, start_);
    start_ = 0;
  }
  buffer_.append(data, size);
}

std::optional<Message> FrameDecoder::next() {
  const std::size_t available = buffer_.size() - start_;
  if (available < 4) return std::nullopt;
  const auto* p = reinterpret_cast<const std::uint8_t*>(buffer_.data() + start_);
  const std::uint32_t length = getU32(p);
  if (length == 0 || length > kMaxBody) {
    throw ProtocolError("frame length " + std::to_string(length) + " is outside 1.." + std::to_string(kMaxBody));
  }
  if (available < 4 + static_cast<std::size_t>(length)) return std::nullopt;
  Message m = decodeBody(std::span<const std::uint8_t>(p + 4, length));
  start_ += 4 + length;
  return m;
}

} // namespace postrack
