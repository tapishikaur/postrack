#pragma once
// A fresh temporary folder per test, removed afterwards.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace postrack::testing {

class TestDir {
public:
  TestDir() {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("postrack-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }
  ~TestDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TestDir(const TestDir&) = delete;
  TestDir& operator=(const TestDir&) = delete;

  std::string file(const std::string& name) const { return (path_ / name).string(); }
  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace postrack::testing
