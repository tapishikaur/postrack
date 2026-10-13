#include "postrack/crc32c.hpp"
#include "postrack/protocol.hpp"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace postrack {
namespace {

Fill sampleFill(std::uint64_t seq) {
  return Fill{seq, 900 + seq, 1'790'000'000'000'000'000LL + static_cast<std::int64_t>(seq), "STRAT01", "SYM07",
              seq % 2 ? Side::Buy : Side::Sell, static_cast<Qty>(seq % 400 + 1), 1'502'500 + static_cast<PriceRaw>(seq)};
}

std::vector<Message> sampleMessages() {
  return {ResumeMsg{12345}, sampleFill(1), MarkMsg{"SYM07", 1'510'000, 77}, HeartbeatMsg{}, EndMsg{99}, sampleFill(2)};
}

std::string stream(const std::vector<Message>& messages) {
  std::string s;
  for (const Message& m : messages) s += encodeFrame(m);
  return s;
}

std::vector<Message> drain(FrameDecoder& d) {
  std::vector<Message> out;
  while (auto m = d.next()) out.push_back(*m);
  return out;
}

TEST(Crc32c, MatchesTheStandardCheckValue) {
  EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
  EXPECT_EQ(crc32c("", 0), 0u);
}

TEST(Protocol, EveryMessageRoundTrips) {
  for (const Message& m : sampleMessages()) {
    FrameDecoder d;
    d.feed(encodeFrame(m));
    const auto got = d.next();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, m);
    EXPECT_EQ(d.buffered(), 0u);
  }
}

TEST(Protocol, FillLayoutIsLittleEndianAndFixed) {
  const std::string f = encodeFrame(Fill{1, 2, 3, "A", "B", Side::Sell, 4, 5});
  // u32 length, type, version, seq, exec_id, ts, str A, str B, side, qty, price
  ASSERT_EQ(f.size(), 4u + 2 + 8 + 8 + 8 + 2 + 2 + 1 + 8 + 8);
  EXPECT_EQ(static_cast<unsigned char>(f[0]), f.size() - 4); // length, low byte first
  EXPECT_EQ(f[4], 2);                                       // type Fill
  EXPECT_EQ(f[5], 1);                                       // version
  EXPECT_EQ(f[6], 1);                                       // seq low byte
  EXPECT_EQ(f[7], 0);
}

// TCP can split the stream anywhere: cut it at every byte, the messages must not change.
TEST(Protocol, SplitAtEveryByteGivesTheSameMessages) {
  const std::vector<Message> messages = sampleMessages();
  const std::string s = stream(messages);
  for (std::size_t cut = 0; cut <= s.size(); ++cut) {
    FrameDecoder d;
    d.feed(s.substr(0, cut));
    std::vector<Message> got = drain(d);
    d.feed(s.substr(cut));
    for (Message& m : drain(d)) got.push_back(std::move(m));
    ASSERT_EQ(got, messages) << "cut at " << cut;
  }
}

TEST(Protocol, RandomChunksGiveTheSameMessages) {
  std::vector<Message> messages;
  for (std::uint64_t i = 1; i <= 2'000; ++i) messages.push_back(sampleFill(i));
  const std::string s = stream(messages);
  std::mt19937_64 rng(5);
  for (int run = 0; run < 20; ++run) {
    FrameDecoder d;
    std::vector<Message> got;
    for (std::size_t pos = 0; pos < s.size();) {
      const std::size_t n = std::min<std::size_t>(s.size() - pos, 1 + rng() % 700);
      d.feed(s.data() + pos, n);
      pos += n;
      for (Message& m : drain(d)) got.push_back(std::move(m));
    }
    ASSERT_EQ(got, messages);
  }
}

std::string frameOf(const std::string& body) {
  std::string f;
  putU32(f, static_cast<std::uint32_t>(body.size()));
  return f + body;
}

void expectRejected(const std::string& bytes, const char* why) {
  FrameDecoder d;
  d.feed(bytes);
  EXPECT_THROW(d.next(), ProtocolError) << why;
}

TEST(Protocol, BadFramesAreRejected) {
  std::string zero;
  putU32(zero, 0);
  expectRejected(zero, "zero length");
  std::string huge;
  putU32(huge, kMaxBody + 1);
  expectRejected(huge, "longer than the maximum, so it can't make the reader wait for 4 GB");
  expectRejected(frameOf(std::string("\x09\x01", 2)), "unknown type");
  expectRejected(frameOf(std::string("\x04\x02", 2)), "unknown version");
  expectRejected(frameOf(std::string("\x04\x01\x00", 3)), "bytes left over");
  expectRejected(frameOf(std::string("\x01\x01\x05", 3)), "body ends inside a field");

  std::string body;
  encodeBody(sampleFill(3), body);
  body[body.size() - 17] = 3; // side byte: neither 1 nor 2
  expectRejected(frameOf(body), "bad side");
}

TEST(Protocol, EncodingRefusesBadFields) {
  std::string out;
  Fill f = sampleFill(1);
  f.qty = 0;
  EXPECT_THROW(encodeBody(f, out), ProtocolError);
  f = sampleFill(1);
  f.symbol = std::string(65, 'X');
  EXPECT_THROW(encodeBody(f, out), ProtocolError);
  f = sampleFill(1);
  f.strategy.clear();
  EXPECT_THROW(encodeBody(f, out), ProtocolError);
  EXPECT_THROW(encodeBody(MarkMsg{"S", 0, 0}, out), ProtocolError);
}

// Garbage must only ever produce ProtocolError, never a crash or a hang.
TEST(Protocol, RandomGarbageNeverCrashes) {
  std::mt19937_64 rng(11);
  for (int run = 0; run < 20'000; ++run) {
    std::string bytes(1 + rng() % 64, '\0');
    for (char& c : bytes) c = static_cast<char>(rng());
    if (run % 2 && bytes.size() >= 4) { // a plausible length, so the body decoder sees the garbage
      bytes[0] = static_cast<char>(bytes.size() - 4);
      bytes[1] = bytes[2] = bytes[3] = 0;
    }
    FrameDecoder d;
    d.feed(bytes);
    try {
      while (d.next()) {
      }
    } catch (const ProtocolError&) {
    }
  }
  SUCCEED();
}

} // namespace
} // namespace postrack
