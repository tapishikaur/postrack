#include "postrack/service.hpp"

#include "postrack/protocol.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>

namespace postrack {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

constexpr auto kMinBackoff = std::chrono::milliseconds(50);
constexpr auto kMaxBackoff = std::chrono::milliseconds(2000);

} // namespace

struct Service::Impl : std::enable_shared_from_this<Service::Impl> {
  // One TCP connection (upstream or price feed) with its own decoder and reconnect timer.
  struct Link {
    explicit Link(asio::io_context& io) : resolver(io), socket(io), timer(io) {}
    tcp::resolver resolver;
    tcp::socket socket;
    asio::steady_timer timer;
    FrameDecoder decoder;
    std::array<char, 1 << 16> in{};
    std::string out;
    std::chrono::milliseconds backoff = kMinBackoff;

    void close() {
      boost::system::error_code ignored;
      resolver.cancel();
      timer.cancel();
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
    }
  };

  asio::io_context& io;
  ServiceConfig config;
  SnapshotStore& store;

  PositionBook book;
  std::optional<FillLog> log;
  std::uint64_t loggedSeq = 0;
  std::uint64_t appliedSeq = 0;
  std::uint64_t lastSnapshotSeq = 0;
  std::uint64_t nextProgress = 100'000;
  bool recovered = false;
  bool stopped = false;
  bool marksEnded = false;
  ServiceStats stats;

  Link upstream;
  Link prices;

  Impl(asio::io_context& ctx, ServiceConfig cfg, SnapshotStore& s)
      : io(ctx), config(std::move(cfg)), store(s), upstream(ctx), prices(ctx) {}

  void say(const std::string& text) const {
    if (!config.quiet) std::cout << "postrack: " << text << std::endl;
  }

  // ── Recovery ──────────────────────────────────────────────────────────────
  RecoveryStats recover() {
    RecoveryStats r;
    auto t0 = Clock::now();
    if (std::optional<Snapshot> snapshot = store.loadLatest()) {
      restoreSnapshot(*snapshot, book);
      r.fromSnapshot = true;
      r.snapshotSeq = snapshot->lastSeq;
    }
    r.snapshotMs = msSince(t0);
    lastSnapshotSeq = r.snapshotSeq;

    t0 = Clock::now();
    LogOpenStats ls;
    log.emplace(FillLog::open(config.logPath, config.sync,
                              [&](const Fill& f) {
                                if (f.seq > r.snapshotSeq) {
                                  book.apply(f);
                                  ++r.replayed;
                                }
                              },
                              &ls));
    r.replayMs = msSince(t0);
    r.logRecords = ls.records;
    r.truncatedBytes = ls.truncatedBytes;

    // Normally the log holds everything the snapshot does and more. If it holds less (it
    // was written with --sync none and the machine crashed), the upstream resends the
    // missing fills: they go into the log but aren't applied a second time.
    loggedSeq = ls.lastSeq;
    appliedSeq = std::max(r.snapshotSeq, loggedSeq);
    nextProgress = (appliedSeq / 100'000 + 1) * 100'000;
    recovered = true;
    return r;
  }

  // ── Upstream ──────────────────────────────────────────────────────────────
  void connectUpstream() {
    auto self = shared_from_this();
    upstream.resolver.async_resolve(
        config.upstreamHost, std::to_string(config.upstreamPort),
        [self](boost::system::error_code ec, const tcp::resolver::results_type& endpoints) {
          if (self->stopped) return;
          if (ec) return self->retryUpstream(true);
          asio::async_connect(self->upstream.socket, endpoints,
                              [self](boost::system::error_code ec2, const tcp::endpoint&) {
                                if (self->stopped) return;
                                if (ec2) return self->retryUpstream(true);
                                ++self->stats.connects;
                                self->upstream.backoff = kMinBackoff;
                                self->upstream.decoder = FrameDecoder{};
                                self->upstream.out = encodeFrame(ResumeMsg{self->loggedSeq});
                                asio::async_write(self->upstream.socket, asio::buffer(self->upstream.out),
                                                  [self](boost::system::error_code ec3, std::size_t) {
                                                    if (self->stopped) return;
                                                    if (ec3) return self->retryUpstream(true);
                                                    self->readUpstream();
                                                  });
                              });
        });
  }

  // Drops the connection and reconnects: at once after a gap, with back-off after errors.
  void retryUpstream(bool backOff) {
    if (stopped) return;
    boost::system::error_code ignored;
    upstream.socket.close(ignored);
    upstream.socket = tcp::socket(io);
    const auto delay = backOff ? upstream.backoff : std::chrono::milliseconds(0);
    if (backOff) upstream.backoff = std::min(upstream.backoff * 2, kMaxBackoff);
    upstream.timer.expires_after(delay);
    auto self = shared_from_this();
    upstream.timer.async_wait([self](boost::system::error_code ec) {
      if (!ec && !self->stopped) self->connectUpstream();
    });
  }

  void readUpstream() {
    auto self = shared_from_this();
    upstream.socket.async_read_some(asio::buffer(upstream.in), [self](boost::system::error_code ec, std::size_t n) {
      if (self->stopped) return;
      if (ec) return self->retryUpstream(true);
      self->onUpstreamData(n);
    });
  }

  void onUpstreamData(std::size_t n) {
    upstream.decoder.feed(upstream.in.data(), n);
    bool gap = false;
    bool appended = false;
    try {
      while (std::optional<Message> m = upstream.decoder.next()) {
        if (const Fill* f = std::get_if<Fill>(&*m)) {
          if (f->seq <= loggedSeq) {
            ++stats.duplicates;
            continue;
          }
          if (f->seq != loggedSeq + 1) {
            ++stats.gaps;
            gap = true;
            break;
          }
          log->append(*f); // logged first...
          appended = true;
          loggedSeq = f->seq;
          if (f->seq > appliedSeq) { // ...then applied, exactly once
            book.apply(*f);
            appliedSeq = f->seq;
            ++stats.fillsApplied;
          }
        } else if (!std::holds_alternative<HeartbeatMsg>(*m) && !std::holds_alternative<EndMsg>(*m)) {
          throw ProtocolError("unexpected message type from the upstream");
        }
      }
    } catch (const ProtocolError& e) {
      ++stats.protocolErrors;
      say(std::string("protocol error from upstream: ") + e.what() + "; reconnecting");
      if (appended) log->sync();
      return retryUpstream(true);
    }
    if (appended) log->sync(); // one disk flush per batch read from the socket
    maybeSnapshot(false);
    progress();
    if (checkFinished()) return;
    if (gap) return retryUpstream(false);
    readUpstream();
  }

  // ── Price feed ────────────────────────────────────────────────────────────
  void connectPrices() {
    auto self = shared_from_this();
    prices.resolver.async_resolve(
        config.pricesHost, std::to_string(config.pricesPort),
        [self](boost::system::error_code ec, const tcp::resolver::results_type& endpoints) {
          if (self->stopped) return;
          if (ec) return self->retryPrices();
          asio::async_connect(self->prices.socket, endpoints, [self](boost::system::error_code ec2, const tcp::endpoint&) {
            if (self->stopped) return;
            if (ec2) return self->retryPrices();
            self->prices.backoff = kMinBackoff;
            self->prices.decoder = FrameDecoder{};
            self->prices.out = encodeFrame(ResumeMsg{0});
            asio::async_write(self->prices.socket, asio::buffer(self->prices.out),
                              [self](boost::system::error_code ec3, std::size_t) {
                                if (self->stopped) return;
                                if (ec3) return self->retryPrices();
                                self->readPrices();
                              });
          });
        });
  }

  void retryPrices() {
    if (stopped || marksEnded) return;
    boost::system::error_code ignored;
    prices.socket.close(ignored);
    prices.socket = tcp::socket(io);
    prices.timer.expires_after(prices.backoff);
    prices.backoff = std::min(prices.backoff * 2, kMaxBackoff);
    auto self = shared_from_this();
    prices.timer.async_wait([self](boost::system::error_code ec) {
      if (!ec && !self->stopped) self->connectPrices();
    });
  }

  void readPrices() {
    auto self = shared_from_this();
    prices.socket.async_read_some(asio::buffer(prices.in), [self](boost::system::error_code ec, std::size_t n) {
      if (self->stopped) return;
      if (ec) return self->retryPrices();
      self->prices.decoder.feed(self->prices.in.data(), n);
      try {
        while (std::optional<Message> m = self->prices.decoder.next()) {
          if (const MarkMsg* mark = std::get_if<MarkMsg>(&*m)) {
            self->book.setMark(mark->symbol, mark->price);
            ++self->stats.marks;
          } else if (std::holds_alternative<EndMsg>(*m)) {
            self->marksEnded = true;
          } else if (!std::holds_alternative<HeartbeatMsg>(*m)) {
            throw ProtocolError("unexpected message type from the price feed");
          }
        }
      } catch (const ProtocolError& e) {
        ++self->stats.protocolErrors;
        self->say(std::string("protocol error from price feed: ") + e.what() + "; reconnecting");
        return self->retryPrices();
      }
      if (self->checkFinished()) return;
      self->readPrices();
    });
  }

  // ── Snapshots, progress, stop ─────────────────────────────────────────────
  void maybeSnapshot(bool force) {
    if (appliedSeq == lastSnapshotSeq) return;
    if (!force && (config.snapshotEvery == 0 || appliedSeq - lastSnapshotSeq < config.snapshotEvery)) return;
    log->sync(); // never snapshot fills that aren't safely in the log
    try {
      store.save(takeSnapshot(book, appliedSeq));
      lastSnapshotSeq = appliedSeq;
      ++stats.snapshots;
    } catch (const std::exception& e) {
      // The log still has everything: recovery just replays more. Try again later.
      ++stats.snapshotFailures;
      say(std::string("snapshot failed (will retry): ") + e.what());
    }
  }

  void progress() {
    if (appliedSeq < nextProgress) return;
    nextProgress = (appliedSeq / 100'000 + 1) * 100'000;
    say("applied seq " + std::to_string(appliedSeq) + " (duplicates ignored " + std::to_string(stats.duplicates) +
        ", gaps " + std::to_string(stats.gaps) + ", snapshots " + std::to_string(stats.snapshots) + ")");
  }

  bool checkFinished() {
    if (stopped) return true;
    if (config.stopAfterSeq == 0 || appliedSeq < config.stopAfterSeq) return false;
    if (config.pricesPort != 0 && !marksEnded) return false;
    stop();
    return true;
  }

  void stop() {
    if (stopped) return;
    stopped = true;
    if (log) {
      log->sync();
      maybeSnapshot(true);
    }
    if (!config.reportDir.empty()) writeReport(config.reportDir);
    upstream.close();
    prices.close();
  }

  void writeReport(const std::string& dir) const {
    std::filesystem::create_directories(dir);
    std::ofstream positions(std::filesystem::path(dir) / "positions.csv");
    std::ofstream lots(std::filesystem::path(dir) / "lots.csv");
    std::ofstream summary(std::filesystem::path(dir) / "summary.txt");
    if (!positions || !lots || !summary) throw std::runtime_error("can't write the report in " + dir);

    positions << "strategy,symbol,net_qty,realized_raw,cash_raw,mark_raw,unrealized_raw,avg_cost_raw,open_lots\n";
    lots << "strategy,symbol,queue_pos,qty,price_raw\n";
    struct Totals {
      Int128 realized = 0, unrealized = 0;
      std::size_t positions = 0;
    };
    std::map<std::string, Totals> byStrategy;
    for (const auto& [key, p] : book.positions()) {
      const std::optional<PriceRaw> mark = book.mark(key.symbol);
      const Int128 unrealized = book.unrealized(key);
      const std::optional<PriceRaw> avg = p.averageCost();
      positions << key.strategy << ',' << key.symbol << ',' << p.netQty() << ',' << toString(p.realized()) << ','
                << toString(p.cash()) << ',' << (mark ? std::to_string(*mark) : "") << ',' << toString(unrealized)
                << ',' << (avg ? std::to_string(*avg) : "") << ',' << p.lots().size() << '\n';
      int queuePos = 0;
      for (const Lot& lot : p.lots()) {
        lots << key.strategy << ',' << key.symbol << ',' << queuePos++ << ',' << lot.qty << ',' << lot.price << '\n';
      }
      Totals& t = byStrategy[key.strategy];
      t.realized += p.realized();
      t.unrealized += unrealized;
      ++t.positions;
    }

    summary << "PosTrack report: positions and P&L per strategy\n"
            << "last applied seq: " << appliedSeq << "\n\n";
    Totals all;
    for (const auto& [strategy, t] : byStrategy) {
      summary << strategy << ": " << t.positions << " positions, realized " << formatMoney(t.realized)
              << ", unrealized " << formatMoney(t.unrealized) << ", total " << formatMoney(t.realized + t.unrealized)
              << '\n';
      all.realized += t.realized;
      all.unrealized += t.unrealized;
      all.positions += t.positions;
    }
    summary << "\nALL: " << all.positions << " positions, realized " << formatMoney(all.realized) << ", unrealized "
            << formatMoney(all.unrealized) << ", total " << formatMoney(all.realized + all.unrealized) << '\n';
  }
};

Service::Service(asio::io_context& io, ServiceConfig config, SnapshotStore& store)
    : impl_(std::make_shared<Impl>(io, std::move(config), store)) {}

Service::~Service() {
  // Close the sockets so pending handlers finish; they hold the Impl alive until then.
  impl_->stopped = true;
  impl_->upstream.close();
  impl_->prices.close();
  impl_->log.reset(); // close the file now, not when the last pending handler lets go
}

RecoveryStats Service::recover() { return impl_->recover(); }

void Service::start() {
  if (!impl_->recovered) throw std::logic_error("call recover() before start()");
  impl_->connectUpstream();
  if (impl_->config.pricesPort != 0) {
    impl_->connectPrices();
  } else {
    impl_->checkFinished(); // already past --stop-after-seq after recovery: nothing to wait for
  }
}

void Service::stop() { impl_->stop(); }
bool Service::stopped() const { return impl_->stopped; }
const PositionBook& Service::book() const { return impl_->book; }
std::uint64_t Service::appliedSeq() const { return impl_->appliedSeq; }
std::uint64_t Service::loggedSeq() const { return impl_->loggedSeq; }
const ServiceStats& Service::stats() const { return impl_->stats; }
void Service::writeReport(const std::string& dir) const { impl_->writeReport(dir); }

} // namespace postrack
