// postrack_export: writes every snapshot in a schema to CSV, for reconcile.py.
//
//   postrack_export --db "host=... dbname=..." --schema postrack --out DIR
//
// Writes DIR/snapshots.csv (last_seq), DIR/snapshot_positions.csv and DIR/snapshot_lots.csv,
// each row tagged with its snapshot's last_seq.

#include "postrack/cli.hpp"
#include "postrack/snapshot.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  using namespace postrack;
  try {
    const Args args(argc, argv, {"db", "schema", "out"});
    if (!args.has("db") || !args.has("out")) throw std::invalid_argument("--db and --out are required");
    PgSnapshotStore store(args.str("db", ""), args.str("schema", "postrack"), 0);
    const std::vector<Snapshot> snapshots = store.loadAll();

    const std::filesystem::path out = args.str("out", "");
    std::filesystem::create_directories(out);
    std::ofstream list(out / "snapshots.csv"), positions(out / "snapshot_positions.csv"), lots(out / "snapshot_lots.csv");
    if (!list || !positions || !lots) throw std::runtime_error("can't write to " + out.string());
    list << "last_seq,positions\n";
    positions << "last_seq,strategy,symbol,net_qty,realized_raw,cash_raw\n";
    lots << "last_seq,strategy,symbol,queue_pos,qty,price_raw\n";
    for (const Snapshot& s : snapshots) {
      list << s.lastSeq << ',' << s.positions.size() << '\n';
      for (const PositionRow& p : s.positions) {
        positions << s.lastSeq << ',' << p.strategy << ',' << p.symbol << ',' << p.netQty << ',' << toString(p.realized)
                  << ',' << toString(p.cash) << '\n';
        int queuePos = 0;
        for (const Lot& lot : p.lots) {
          lots << s.lastSeq << ',' << p.strategy << ',' << p.symbol << ',' << queuePos++ << ',' << lot.qty << ','
               << lot.price << '\n';
        }
      }
    }
    std::cout << "postrack_export: " << snapshots.size() << " snapshots written to " << out.string() << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "postrack_export: " << e.what() << '\n';
    return 1;
  }
}
