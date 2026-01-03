// bt_dsl/driver/stdlib_finder.hpp - Standard library auto-detection
//
// Provides utilities for locating the BT-DSL standard library.
// Used by btc CLI and LSP server.
//
#pragma once

#include <filesystem>
#include <optional>

namespace bt_dsl
{

/**
 * Try to find the standard library in standard locations.
 *
 * Search order:
 * 1. Installed path (from cmake install, BT_DSL_STDLIB_INSTALL_PATH)
 * 2. Relative to executable: <prefix>/share/bt-dsl/std/
 * 3. Development layout: <build>/../std/
 *
 * @return Path to stdlib directory, or nullopt if not found
 */
[[nodiscard]] std::optional<std::filesystem::path> find_stdlib();

}  // namespace bt_dsl
