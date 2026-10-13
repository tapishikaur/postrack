#pragma once
// A very small "--name value" argument parser for the command-line tools.

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace postrack {

class Args {
public:
  // `known` lists the accepted option names (without "--"); flags take no value.
  Args(int argc, char** argv, std::set<std::string> known, std::set<std::string> flags = {}) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument '" + a + "'");
      a.erase(0, 2);
      if (flags.contains(a)) {
        values_[a] = "1";
        continue;
      }
      if (!known.contains(a)) throw std::invalid_argument("unknown option --" + a);
      if (i + 1 >= argc) throw std::invalid_argument("--" + a + " needs a value");
      values_[a] = argv[++i];
    }
  }

  bool has(const std::string& name) const { return values_.contains(name); }
  std::string str(const std::string& name, const std::string& fallback) const {
    const auto found = values_.find(name);
    return found == values_.end() ? fallback : found->second;
  }
  std::uint64_t u64(const std::string& name, std::uint64_t fallback) const {
    const auto found = values_.find(name);
    if (found == values_.end()) return fallback;
    std::size_t used = 0;
    const unsigned long long v = std::stoull(found->second, &used);
    if (used != found->second.size()) throw std::invalid_argument("--" + name + " must be a whole number");
    return v;
  }
  double real(const std::string& name, double fallback) const {
    const auto found = values_.find(name);
    return found == values_.end() ? fallback : std::stod(found->second);
  }

  // "host:port" -> host and port.
  static std::pair<std::string, std::uint16_t> endpoint(const std::string& text) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument("expected host:port, got '" + text + "'");
    const unsigned long port = std::stoul(text.substr(colon + 1));
    if (port == 0 || port > 65535) throw std::invalid_argument("bad port in '" + text + "'");
    return {text.substr(0, colon), static_cast<std::uint16_t>(port)};
  }

private:
  std::map<std::string, std::string> values_;
};

} // namespace postrack
