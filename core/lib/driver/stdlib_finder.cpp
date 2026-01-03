// bt_dsl/driver/stdlib_finder.cpp - Standard library auto-detection
//
#include "bt_dsl/driver/stdlib_finder.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace bt_dsl
{

std::optional<fs::path> find_stdlib()
{
  // 1. Check installed path (from cmake install)
#ifdef BT_DSL_STDLIB_INSTALL_PATH
  {
    fs::path installed = BT_DSL_STDLIB_INSTALL_PATH;
    if (fs::is_directory(installed)) {
      return installed;
    }
  }
#endif

  // 2. Check relative to executable location
  // Typical layout:
  //   Installed: <prefix>/bin/btc, <prefix>/share/bt-dsl/std/
  //   Development: <build>/btc, <project>/std/
  {
    std::error_code ec;

#ifdef _WIN32
    // Windows: use GetModuleFileName
    wchar_t path_buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path_buf, MAX_PATH) != 0) {
      fs::path exe_path(path_buf);
#else
    // Linux/macOS: use /proc/self/exe or _NSGetExecutablePath
    auto exe_path = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
#endif
      auto bin_dir = exe_path.parent_path();

      // Check relative to bin: ../share/bt-dsl/std/
      auto share_stdlib = bin_dir / ".." / "share" / "bt-dsl" / "std";
      if (fs::is_directory(share_stdlib, ec)) {
        return fs::canonical(share_stdlib, ec);
      }

      // Check for development: ../std/ (build/btc -> core/std/)
      auto dev_stdlib = bin_dir / ".." / "std";
      if (fs::is_directory(dev_stdlib, ec)) {
        return fs::canonical(dev_stdlib, ec);
      }
    }
  }

  return std::nullopt;
}

}  // namespace bt_dsl
