// fill_simulator: a test upstream. Generates repeatable synthetic fills from a seed and
// serves them over the PosTrack protocol, honouring Resume; runs until killed.
//
//   fill_simulator [--fills 1000000] [--seed 42] [--strategies 8] [--symbols 20]
//                  [--port 9101] [--duplicate-rate 0.001] [--gap-rate 0.0001] [--rate 100000]
//                  [--dump fills.bin] [--prices-out prices.csv] [--mark-every 5000]
//                  [--generate-only]
//
// --dump writes every fill in fill-log format and --prices-out the marks, so the Python
// reference can check PosTrack against exactly what was sent. --rate paces the feed to that
// many fills per second, like a live drop copy (default: as fast as possible).
// --generate-only writes the files and exits without serving.

#include "postrack/cli.hpp"
#include "postrack/sim.hpp"

#include <boost/asio/io_context.hpp>

#include <iostream>

int main(int argc, char** argv) {
  using namespace postrack;
  try {
    const Args args(argc, argv,
                    {"fills", "seed", "strategies", "symbols", "port", "duplicate-rate", "gap-rate", "dump", "prices-out",
                     "mark-every", "rate"},
                    {"generate-only"});
    SimConfig config;
    config.fills = args.u64("fills", config.fills);
    config.seed = args.u64("seed", config.seed);
    config.strategies = static_cast<int>(args.u64("strategies", 8));
    config.symbols = static_cast<int>(args.u64("symbols", 20));
    config.markEvery = args.u64("mark-every", config.markEvery);
    SimData data = generate(config);
    if (args.has("dump")) writeFillDump(data.fills, args.str("dump", ""));
    if (args.has("prices-out")) writePriceFile(data.marks, args.str("prices-out", ""));
    std::cout << "fill_simulator: " << data.fills.size() << " fills, " << data.marks.size() << " marks (seed "
              << config.seed << ")" << std::endl;
    if (args.has("generate-only")) return 0;

    UpstreamOptions options;
    options.port = static_cast<std::uint16_t>(args.u64("port", 9101));
    options.duplicateRate = args.real("duplicate-rate", 0.0);
    options.gapRate = args.real("gap-rate", 0.0);
    options.rate = args.u64("rate", 0);
    boost::asio::io_context io;
    UpstreamServer server(io, std::move(data.fills), options);
    std::cout << "fill_simulator: listening on 127.0.0.1:" << server.port() << std::endl;
    io.run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "fill_simulator: " << e.what() << '\n';
    return 1;
  }
}
