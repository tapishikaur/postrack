#include "postrack/snapshot.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace postrack {
namespace {

// Schema names are put straight into SQL, so allow only a safe set of characters.
std::string checkedSchema(const std::string& schema) {
  const bool ok = !schema.empty() && schema.size() <= 48 &&
                  std::all_of(schema.begin(), schema.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                  }) &&
                  !(schema[0] >= '0' && schema[0] <= '9');
  if (!ok) throw std::invalid_argument("schema name must be lowercase letters, digits and _: '" + schema + "'");
  return schema;
}

constexpr int kSchemaVersion = 1;

// Money is stored as numeric(39,0): exact, any 128-bit value fits.
const char* const kCreateTables = R"sql(
CREATE TABLE IF NOT EXISTS postrack_schema (version integer PRIMARY KEY);
CREATE TABLE IF NOT EXISTS snapshots (
  id         bigserial   PRIMARY KEY,
  last_seq   bigint      NOT NULL CHECK (last_seq >= 0),
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS positions (
  snapshot_id bigint        NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
  strategy    text          NOT NULL,
  symbol      text          NOT NULL,
  net_qty     bigint        NOT NULL,
  realized    numeric(39,0) NOT NULL,
  cash        numeric(39,0) NOT NULL,
  PRIMARY KEY (snapshot_id, strategy, symbol)
);
CREATE TABLE IF NOT EXISTS lots (
  snapshot_id bigint  NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
  strategy    text    NOT NULL,
  symbol      text    NOT NULL,
  queue_pos   integer NOT NULL,
  qty         bigint  NOT NULL CHECK (qty > 0),
  price       bigint  NOT NULL CHECK (price > 0),
  PRIMARY KEY (snapshot_id, strategy, symbol, queue_pos)
);
)sql";

Int128 int128(const pqxx::field& f) {
  const auto v = parseInt128(f.c_str());
  if (!v) throw std::runtime_error(std::string("bad money value in snapshot: ") + f.c_str());
  return *v;
}

} // namespace

struct PgSnapshotStore::Impl {
  pqxx::connection connection;
  std::string schema;
  std::size_t keep;

  Impl(const std::string& conn, const std::string& schemaName, std::size_t keepCount)
      : connection(conn), schema(checkedSchema(schemaName)), keep(keepCount) {
    pqxx::work tx(connection);
    tx.exec("CREATE SCHEMA IF NOT EXISTS " + schema);
    tx.exec("SET LOCAL search_path TO " + schema);
    tx.exec(kCreateTables);
    tx.exec("INSERT INTO postrack_schema (version) VALUES (" + std::to_string(kSchemaVersion) +
            ") ON CONFLICT DO NOTHING");
    tx.commit();
  }

  void usingSchema(pqxx::transaction_base& tx) { tx.exec("SET LOCAL search_path TO " + schema); }

  // Loads the snapshots whose ids are listed, in that order.
  std::vector<Snapshot> load(pqxx::transaction_base& tx, const pqxx::result& ids) {
    std::vector<Snapshot> out;
    std::map<long long, std::size_t> index;
    std::string idList;
    for (const auto& row : ids) {
      const long long id = row[0].as<long long>();
      index[id] = out.size();
      Snapshot s;
      s.lastSeq = row[1].as<std::uint64_t>();
      out.push_back(std::move(s));
      if (!idList.empty()) idList += ",";
      idList += std::to_string(id);
    }
    if (out.empty()) return out;

    // Positions, then their lots, matched by (snapshot, strategy, symbol).
    std::map<std::tuple<long long, std::string, std::string>, PositionRow*> rows;
    const pqxx::result positions =
        tx.exec("SELECT snapshot_id, strategy, symbol, net_qty, realized::text, cash::text FROM positions "
                "WHERE snapshot_id IN (" + idList + ") ORDER BY snapshot_id, strategy, symbol");
    for (const auto& r : positions) {
      const long long id = r[0].as<long long>();
      PositionRow p;
      p.strategy = r[1].as<std::string>();
      p.symbol = r[2].as<std::string>();
      p.netQty = r[3].as<long long>();
      p.realized = int128(r[4]);
      p.cash = int128(r[5]);
      Snapshot& s = out[index.at(id)];
      s.positions.push_back(std::move(p));
    }
    // Pointers into the vectors are taken only now, after every push_back, so they stay valid.
    for (const auto& [id, at] : index) {
      for (PositionRow& p : out[at].positions) rows[{id, p.strategy, p.symbol}] = &p;
    }
    const pqxx::result lots = tx.exec("SELECT snapshot_id, strategy, symbol, qty, price FROM lots WHERE snapshot_id IN (" +
                                      idList + ") ORDER BY snapshot_id, strategy, symbol, queue_pos");
    for (const auto& r : lots) {
      const auto found = rows.find({r[0].as<long long>(), r[1].as<std::string>(), r[2].as<std::string>()});
      if (found == rows.end()) throw std::runtime_error("snapshot lot without a position row");
      found->second->lots.push_back(Lot{r[3].as<long long>(), r[4].as<long long>()});
    }
    return out;
  }
};

PgSnapshotStore::PgSnapshotStore(const std::string& connection, const std::string& schema, std::size_t keep)
    : impl_(std::make_unique<Impl>(connection, schema, keep)) {}

PgSnapshotStore::~PgSnapshotStore() = default;

void PgSnapshotStore::save(const Snapshot& snapshot) {
  pqxx::work tx(impl_->connection);
  impl_->usingSchema(tx);
  const long long id =
      tx.exec("INSERT INTO snapshots (last_seq) VALUES (" + std::to_string(snapshot.lastSeq) + ") RETURNING id")
          .at(0)
          .at(0)
          .as<long long>();

  // COPY is far faster than one INSERT per row.
  {
    auto stream = pqxx::stream_to::table(tx, {"positions"},
                                         {"snapshot_id", "strategy", "symbol", "net_qty", "realized", "cash"});
    for (const PositionRow& p : snapshot.positions) {
      stream.write_values(id, p.strategy, p.symbol, static_cast<long long>(p.netQty), toString(p.realized),
                          toString(p.cash));
    }
    stream.complete();
  }
  {
    auto stream =
        pqxx::stream_to::table(tx, {"lots"}, {"snapshot_id", "strategy", "symbol", "queue_pos", "qty", "price"});
    for (const PositionRow& p : snapshot.positions) {
      int queuePos = 0;
      for (const Lot& lot : p.lots) {
        stream.write_values(id, p.strategy, p.symbol, queuePos++, static_cast<long long>(lot.qty),
                            static_cast<long long>(lot.price));
      }
    }
    stream.complete();
  }
  if (impl_->keep > 0) {
    tx.exec("DELETE FROM snapshots WHERE id NOT IN (SELECT id FROM snapshots ORDER BY id DESC LIMIT " +
            std::to_string(impl_->keep) + ")");
  }
  tx.commit(); // nothing above is visible until here; any exception rolls it all back
}

std::optional<Snapshot> PgSnapshotStore::loadLatest() {
  pqxx::read_transaction tx(impl_->connection);
  impl_->usingSchema(tx);
  const pqxx::result ids = tx.exec("SELECT id, last_seq FROM snapshots ORDER BY id DESC LIMIT 1");
  std::vector<Snapshot> found = impl_->load(tx, ids);
  if (found.empty()) return std::nullopt;
  return std::move(found.front());
}

std::vector<Snapshot> PgSnapshotStore::loadAll() {
  pqxx::read_transaction tx(impl_->connection);
  impl_->usingSchema(tx);
  return impl_->load(tx, tx.exec("SELECT id, last_seq FROM snapshots ORDER BY id"));
}

void PgSnapshotStore::dropSchema(const std::string& connection, const std::string& schema) {
  pqxx::connection c(connection);
  pqxx::work tx(c);
  tx.exec("DROP SCHEMA IF EXISTS " + checkedSchema(schema) + " CASCADE");
  tx.commit();
}

} // namespace postrack
