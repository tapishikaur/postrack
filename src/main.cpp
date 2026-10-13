// postrack: the position and P&L service.
//
//   postrack --upstream 127.0.0.1:9101 [--prices 127.0.0.1:9102] --log fills.log
//            --db "host=localhost port=54329 dbname=postrack user=postrack" [--schema postrack]
//            [--keep 3] [--snapshot-every 10000] [--sync batch|every|none]
//            [--stop-after-seq N] [--report DIR] [--quiet]
//
// --db none keeps snapshots in memory only (recovery then replays the whole log).
// Ctrl+C or SIGTERM stops gracefully: final log flush, snapshot and report.

#include "postrack/cli.hpp"
#include "postrack/service.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <iostream>
#include <memory>
#include <tuple>

int main(int argc, char** argv) {
  using namespace postrack;
  try {
    const Args args(argc, argv,
                    {"upstream", "prices", "log", "db", "schema", "keep", "snapshot-every", "sync", "stop-after-seq",
                     "report"},
                    {"quiet"});
    ServiceConfig config;
    std::tie(config.upstreamHost, config.upstreamPort) = Args::endpoint(args.str("upstream", "127.0.0.1:9101"));
    if (args.has("prices")) std::tie(config.pricesHost, config.pricesPort) = Args::endpoint(args.str("prices", ""));
    config.logPath = args.str("log", "postrack-fills.log");
    config.sync = parseSyncMode(args.str("sync", "batch"));
    config.snapshotEvery = args.u64("snapshot-every", 10'000);
    config.stopAfterSeq = args.u64("stop-after-seq", 0);
    config.reportDir = args.str("report", "");
    config.quiet = args.has("quiet");

    const std::string db = args.str("db", "none");
    std::unique_ptr<SnapshotStore> store;
    if (db == "none") {
      store = std::make_unique<MemorySnapshotStore>();
    } else {
      store = std::make_unique<PgSnapshotStore>(db, args.str("schema", "postrack"), args.u64("keep", 3));
    }

    boost::asio::io_context io;
    Service service(io, config, *store);

    const RecoveryStats r = service.recover();
    std::cout << "postrack: recovered "
              << (r.fromSnapshot ? "snapshot at seq " + std::to_string(r.snapshotSeq) : std::string("no snapshot"))
              << " in " << r.snapshotMs << " ms; fill log has " << r.logRecords << " records, replayed " << r.replayed
              << " in " << r.replayMs << " ms" << (r.truncatedBytes ? "; cut a torn tail of " : "")
              << (r.truncatedBytes ? std::to_string(r.truncatedBytes) + " bytes" : "") << "; resuming after seq "
              << service.loggedSeq() << std::endl;

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& ec, int) {
      if (!ec) service.stop();
    });
    service.start();
    // Once the service stops, nothing but the signal wait is left: cancel it, then let the
    // remaining handlers finish.
    while (!service.stopped() && io.run_one() > 0) {
    }
    signals.cancel();
    io.run();

    const ServiceStats& s = service.stats();
    std::cout << "postrack: stopped at seq " << service.appliedSeq() << "; this run applied " << s.fillsApplied
              << ", ignored " << s.duplicates << " duplicates, recovered " << s.gaps << " gaps, connected "
              << s.connects << " times, wrote " << s.snapshots << " snapshots (" << s.snapshotFailures
              << " failed), " << s.marks << " marks" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "postrack: " << e.what() << '\n';
    return 1;
  }
}
