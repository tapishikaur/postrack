#pragma once
// Test upstreams: a deterministic fill generator, the fill server (honours Resume, can
// inject duplicates and gaps on purpose) and the price feed server. Used by the
// fill_simulator and price_replayer tools and by the end-to-end tests.

#include "postrack/protocol.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace postrack {

struct SimConfig {
  std::uint64_t seed = 42;
  std::uint64_t fills = 1'000'000;
  int strategies = 8;
  int symbols = 20;
  std::uint64_t markEvery = 5'000; // a mark for every symbol after this many fills
};

struct SimData {
  std::vector<Fill> fills;     // seq 1..N
  std::vector<MarkMsg> marks;  // in time order; the last mark per symbol is the final one
};

// Same config, same data, on every platform (fixed-width integers, no floating point).
SimData generate(const SimConfig& config);

// Files: fills in fill-log record format (docs/protocol.md), marks as "ts_ns,symbol,price".
void writeFillDump(const std::vector<Fill>& fills, const std::string& path);
void writePriceFile(const std::vector<MarkMsg>& marks, const std::string& path);
std::vector<MarkMsg> readPriceFile(const std::string& path);

struct UpstreamOptions {
  std::uint16_t port = 0;        // 0 picks a free port; see port()
  double duplicateRate = 0.0;    // chance, per fill, of also resending the previous one
  double gapRate = 0.0;          // chance, per fill, of skipping it (once) to make a gap
  std::uint64_t chaosSeed = 7;
  std::size_t batch = 512;       // fills per socket write
  std::uint64_t rate = 0;        // fills per second, like a live feed; 0 = as fast as possible
};

// Serves `fills` to one client at a time: reads Resume{last}, sends every fill after it.
class UpstreamServer {
public:
  UpstreamServer(boost::asio::io_context& io, std::vector<Fill> fills, UpstreamOptions options);
  ~UpstreamServer();
  std::uint16_t port() const;
  std::uint64_t connections() const;
  std::uint64_t duplicatesSent() const;
  std::uint64_t gapsMade() const;
  void close();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// Serves marks: on every connection plays them all from the start, then End.
class PriceServer {
public:
  PriceServer(boost::asio::io_context& io, std::vector<MarkMsg> marks, std::uint16_t port = 0);
  ~PriceServer();
  std::uint16_t port() const;
  void close();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace postrack
