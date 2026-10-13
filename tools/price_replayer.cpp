// price_replayer: plays a price file ("ts_ns,symbol,price" per line) as Mark messages to
// every client that connects, then End. Runs until killed.
//
//   price_replayer --prices prices.csv [--port 9102]

#include "postrack/cli.hpp"
#include "postrack/sim.hpp"

#include <boost/asio/io_context.hpp>

#include <iostream>

int main(int argc, char** argv) {
  using namespace postrack;
  try {
    const Args args(argc, argv, {"prices", "port"});
    if (!args.has("prices")) throw std::invalid_argument("--prices is required");
    std::vector<MarkMsg> marks = readPriceFile(args.str("prices", ""));
    boost::asio::io_context io;
    PriceServer server(io, std::move(marks), static_cast<std::uint16_t>(args.u64("port", 9102)));
    std::cout << "price_replayer: listening on 127.0.0.1:" << server.port() << std::endl;
    io.run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "price_replayer: " << e.what() << '\n';
    return 1;
  }
}
