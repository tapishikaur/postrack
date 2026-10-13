#include "postrack/sim.hpp"

#include "postrack/crc32c.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace postrack {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

// SplitMix64: tiny, fast, and the same everywhere (unlike std:: distributions).
class Rng {
public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; } // n is small: the bias is negligible
  bool chance(double p) { return p > 0 && static_cast<double>(next() >> 11) * 0x1.0p-53 < p; }

private:
  std::uint64_t state_;
};

std::string name(const char* prefix, int i) {
  std::string n = std::to_string(i + 1);
  if (n.size() < 2) n.insert(0, "0");
  return prefix + n;
}

constexpr std::int64_t kBaseTs = 1'790'000'000'000'000'000LL; // a fixed start time, ns
constexpr PriceRaw kTick = 100;                               // 0.01
constexpr PriceRaw kFloor = 100 * kTick;                      // prices never go below 1.00

} // namespace

SimData generate(const SimConfig& config) {
  if (config.strategies < 1 || config.symbols < 1) throw std::invalid_argument("need at least one strategy and symbol");
  Rng rng(config.seed);
  std::vector<std::string> strategies, symbols;
  for (int i = 0; i < config.strategies; ++i) strategies.push_back(name("STRAT", i));
  for (int i = 0; i < config.symbols; ++i) symbols.push_back(name("SYM", i));

  // Each symbol's price is a random walk in whole ticks, starting at 50.00, 57.00, 64.00, ...
  std::vector<PriceRaw> price(symbols.size());
  for (std::size_t i = 0; i < symbols.size(); ++i) price[i] = (50 + 7 * static_cast<PriceRaw>(i)) * kPriceScale;

  SimData data;
  data.fills.reserve(config.fills);
  std::int64_t ts = kBaseTs;
  const auto markAll = [&] {
    for (std::size_t i = 0; i < symbols.size(); ++i) data.marks.push_back({symbols[i], price[i], ts});
  };
  markAll(); // a starting mark for every symbol

  for (std::uint64_t seq = 1; seq <= config.fills; ++seq) {
    ts += 1'000 + static_cast<std::int64_t>(rng.below(50'000));
    const std::size_t sym = rng.below(symbols.size());
    const std::int64_t step = static_cast<std::int64_t>(rng.below(11)) - 5; // -5..+5 ticks
    price[sym] = std::max(kFloor, price[sym] + step * kTick);

    Fill f;
    f.seq = seq;
    f.execId = 1'000'000'000ULL + seq;
    f.tsNs = ts;
    f.strategy = strategies[rng.below(strategies.size())];
    f.symbol = symbols[sym];
    f.side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
    f.qty = 1 + static_cast<Qty>(rng.below(500));
    f.price = price[sym];
    data.fills.push_back(std::move(f));

    if (config.markEvery > 0 && seq % config.markEvery == 0) markAll();
  }
  ts += 1'000;
  markAll(); // the final marks
  return data;
}

void writeFillDump(const std::vector<Fill>& fills, const std::string& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("can't write " + path);
  std::string record, body;
  for (const Fill& f : fills) {
    body.clear();
    encodeBody(f, body);
    record.clear();
    putU32(record, static_cast<std::uint32_t>(body.size()));
    putU32(record, crc32c(body.data(), body.size()));
    record += body;
    out.write(record.data(), static_cast<std::streamsize>(record.size()));
  }
  if (!out) throw std::runtime_error("can't write " + path);
}

void writePriceFile(const std::vector<MarkMsg>& marks, const std::string& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("can't write " + path);
  out << "ts_ns,symbol,price\n";
  for (const MarkMsg& m : marks) out << m.tsNs << ',' << m.symbol << ',' << formatPrice(m.price) << '\n';
  if (!out) throw std::runtime_error("can't write " + path);
}

std::vector<MarkMsg> readPriceFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("can't open " + path);
  std::vector<MarkMsg> marks;
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (lineNo == 1 || line.empty()) continue; // header
    std::stringstream fields(line);
    std::string ts, symbol, price;
    if (!std::getline(fields, ts, ',') || !std::getline(fields, symbol, ',') || !std::getline(fields, price)) {
      throw std::runtime_error(path + " line " + std::to_string(lineNo) + ": expected ts_ns,symbol,price");
    }
    const auto p = parsePrice(price);
    if (!p || symbol.empty()) throw std::runtime_error(path + " line " + std::to_string(lineNo) + ": bad mark");
    marks.push_back({symbol, *p, std::stoll(ts)});
  }
  return marks;
}

// ── Upstream fill server ─────────────────────────────────────────────────────

struct UpstreamServer::Impl : std::enable_shared_from_this<UpstreamServer::Impl> {
  struct Session : std::enable_shared_from_this<Session> {
    std::shared_ptr<Impl> server;
    tcp::socket socket;
    std::array<char, 4096> in{};
    FrameDecoder decoder;
    std::string out;
    std::uint64_t next = 0; // seq to send next
    Rng rng;
    bool open = true;

    asio::steady_timer pace;
    std::chrono::steady_clock::time_point started{};
    std::uint64_t sentThisSession = 0;

    Session(std::shared_ptr<Impl> s, tcp::socket sock, std::uint64_t seed)
        : server(std::move(s)), socket(std::move(sock)), rng(seed), pace(socket.get_executor()) {}

    void start() { readResume(); }

    void readResume() {
      auto self = shared_from_this();
      socket.async_read_some(asio::buffer(in), [self](boost::system::error_code ec, std::size_t n) {
        if (ec) return self->close();
        try {
          self->decoder.feed(self->in.data(), n);
          if (auto m = self->decoder.next()) {
            const auto* resume = std::get_if<ResumeMsg>(&*m);
            if (!resume) return self->close();
            self->next = resume->lastSeq + 1;
            self->started = std::chrono::steady_clock::now();
            self->sendMore();
            self->watchForClose();
            return;
          }
        } catch (const ProtocolError&) {
          return self->close();
        }
        self->readResume();
      });
    }

    // Keeps a read pending, so a client that disconnects is noticed.
    void watchForClose() {
      auto self = shared_from_this();
      socket.async_read_some(asio::buffer(in), [self](boost::system::error_code ec, std::size_t) {
        if (ec) return self->close();
        self->watchForClose(); // anything else the client sends (heartbeats) is ignored
      });
    }

    void sendMore() {
      const std::vector<Fill>& fills = server->fills;
      out.clear();
      std::size_t frames = 0;
      while (next <= fills.size() && frames < server->options.batch) {
        const Fill& f = fills[next - 1];
        if (server->options.gapRate > 0 && !server->gapped.contains(f.seq) && rng.chance(server->options.gapRate)) {
          server->gapped.insert(f.seq); // skip it this once: the client sees a gap
          ++server->gapsMade;
          ++next;
          continue;
        }
        out += encodeFrame(f);
        ++frames;
        if (next > 1 && rng.chance(server->options.duplicateRate)) {
          out += encodeFrame(fills[next - 2]); // an extra copy of the previous fill
          ++server->duplicatesSent;
        }
        ++next;
      }
      if (out.empty()) return; // everything sent; stay connected for whatever comes next
      sentThisSession += frames;
      auto self = shared_from_this();
      asio::async_write(socket, asio::buffer(out), [self](boost::system::error_code ec, std::size_t) {
        if (ec) return self->close();
        const std::uint64_t rate = self->server->options.rate;
        if (rate == 0) return self->sendMore();
        // Pace to `rate` fills per second: wait until this many fills are due.
        self->pace.expires_at(self->started + std::chrono::microseconds(self->sentThisSession * 1'000'000 / rate));
        self->pace.async_wait([self](boost::system::error_code ec2) {
          if (!ec2 && self->open) self->sendMore();
        });
      });
    }

    void close() {
      if (!open) return;
      open = false;
      pace.cancel();
      boost::system::error_code ignored;
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
    }
  };

  asio::io_context& io;
  tcp::acceptor acceptor;
  std::vector<Fill> fills;
  UpstreamOptions options;
  std::unordered_set<std::uint64_t> gapped;
  std::weak_ptr<Session> current;
  std::uint64_t connections = 0, duplicatesSent = 0, gapsMade = 0;

  Impl(asio::io_context& ctx, std::vector<Fill> f, UpstreamOptions o)
      : io(ctx), acceptor(ctx, tcp::endpoint(asio::ip::make_address("127.0.0.1"), o.port)), fills(std::move(f)),
        options(o) {}

  void accept() {
    auto self = shared_from_this();
    acceptor.async_accept([self](boost::system::error_code ec, tcp::socket socket) {
      if (ec) return; // closed
      if (auto old = self->current.lock()) old->close(); // one client at a time
      ++self->connections;
      auto session = std::make_shared<Session>(self, std::move(socket), self->options.chaosSeed + self->connections);
      self->current = session;
      session->start();
      self->accept();
    });
  }

  void close() {
    boost::system::error_code ignored;
    acceptor.close(ignored);
    if (auto s = current.lock()) s->close();
  }
};

UpstreamServer::UpstreamServer(asio::io_context& io, std::vector<Fill> fills, UpstreamOptions options)
    : impl_(std::make_shared<Impl>(io, std::move(fills), options)) {
  impl_->accept();
}
UpstreamServer::~UpstreamServer() { impl_->close(); }
std::uint16_t UpstreamServer::port() const { return impl_->acceptor.local_endpoint().port(); }
std::uint64_t UpstreamServer::connections() const { return impl_->connections; }
std::uint64_t UpstreamServer::duplicatesSent() const { return impl_->duplicatesSent; }
std::uint64_t UpstreamServer::gapsMade() const { return impl_->gapsMade; }
void UpstreamServer::close() { impl_->close(); }

// ── Price feed server ────────────────────────────────────────────────────────

struct PriceServer::Impl : std::enable_shared_from_this<PriceServer::Impl> {
  struct Session : std::enable_shared_from_this<Session> {
    std::shared_ptr<Impl> server;
    tcp::socket socket;
    std::string out;
    std::size_t next = 0;
    bool endSent = false;
    std::array<char, 1024> in{};

    Session(std::shared_ptr<Impl> s, tcp::socket sock) : server(std::move(s)), socket(std::move(sock)) {}

    void start() {
      sendMore();
      drain();
    }
    void drain() { // reads and ignores what the client sends; notices a disconnect
      auto self = shared_from_this();
      socket.async_read_some(asio::buffer(in), [self](boost::system::error_code ec, std::size_t) {
        if (!ec) self->drain();
      });
    }
    void sendMore() {
      out.clear();
      const auto& marks = server->marks;
      for (int i = 0; i < 512 && next < marks.size(); ++i) out += encodeFrame(marks[next++]);
      if (out.empty()) {
        if (endSent) return;
        endSent = true;
        out = encodeFrame(EndMsg{0});
      }
      auto self = shared_from_this();
      asio::async_write(socket, asio::buffer(out), [self](boost::system::error_code ec, std::size_t) {
        if (!ec) self->sendMore();
      });
    }
  };

  tcp::acceptor acceptor;
  std::vector<MarkMsg> marks;

  Impl(asio::io_context& io, std::vector<MarkMsg> m, std::uint16_t port)
      : acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)), marks(std::move(m)) {}

  void accept() {
    auto self = shared_from_this();
    acceptor.async_accept([self](boost::system::error_code ec, tcp::socket socket) {
      if (ec) return;
      std::make_shared<Session>(self, std::move(socket))->start();
      self->accept();
    });
  }
};

PriceServer::PriceServer(asio::io_context& io, std::vector<MarkMsg> marks, std::uint16_t port)
    : impl_(std::make_shared<Impl>(io, std::move(marks), port)) {
  impl_->accept();
}
PriceServer::~PriceServer() { close(); }
std::uint16_t PriceServer::port() const { return impl_->acceptor.local_endpoint().port(); }
void PriceServer::close() {
  boost::system::error_code ignored;
  impl_->acceptor.close(ignored);
}

} // namespace postrack
