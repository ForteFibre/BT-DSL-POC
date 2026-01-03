// bt_dsl/project/project_config.hpp - Project configuration (btc.yaml)
//
// Parses and validates btc.yaml project configuration files.
// Designed for reuse in both CLI and LSP.
//
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bt_dsl
{

// ============================================================================
// Configuration Structures
// ============================================================================

/**
 * Dependency configuration for a single dependency entry.
 */
struct DependencyConfig
{
  /// Local path dependency (relative to btc.yaml)
  std::optional<std::filesystem::path> path;

  /// ROS package dependency
  std::optional<std::string> ros_package;
};

/**
 * Compiler configuration section.
 */
struct CompilerConfig
{
  /// Entry point files to compile
  std::vector<std::filesystem::path> entry_points;

  /// Output directory for generated files
  std::filesystem::path output_dir = "generated";

  /// Target environment: "btcpp_v4" | "btcpp_v4_strict"
  std::string target = "btcpp_v4";
};

/**
 * Package metadata section.
 */
struct PackageConfig
{
  std::string name;
  std::string version;
};

/**
 * Complete project configuration (btc.yaml).
 */
struct ProjectConfig
{
  PackageConfig package;
  CompilerConfig compiler;
  std::vector<DependencyConfig> dependencies;

  /// Directory containing btc.yaml (for resolving relative paths)
  std::filesystem::path project_root;
};

// ============================================================================
// Configuration Loading Result
// ============================================================================

/**
 * Result of loading a project configuration file.
 */
struct ConfigLoadResult
{
  /// Loaded configuration (only valid if success == true)
  ProjectConfig config;

  /// Whether loading succeeded
  bool success = false;

  /// Error message if loading failed
  std::string error;

  /// Create a successful result
  static ConfigLoadResult ok(ProjectConfig cfg)
  {
    ConfigLoadResult r;
    r.config = std::move(cfg);
    r.success = true;
    return r;
  }

  /// Create a failed result
  static ConfigLoadResult fail(std::string msg)
  {
    ConfigLoadResult r;
    r.error = std::move(msg);
    r.success = false;
    return r;
  }
};

// ============================================================================
// Configuration Loading API
// ============================================================================

/**
 * Load a project configuration from a btc.yaml file.
 *
 * @param config_path Path to btc.yaml
 * @return ConfigLoadResult with the loaded config or error message
 */
[[nodiscard]] ConfigLoadResult load_project_config(const std::filesystem::path & config_path);

/**
 * Find a project configuration file by searching upward from a directory.
 *
 * Searches for btc.yaml starting from start_dir and moving up the
 * directory hierarchy until the filesystem root.
 *
 * @param start_dir Directory to start searching from
 * @return Path to btc.yaml if found, std::nullopt otherwise
 */
[[nodiscard]] std::optional<std::filesystem::path> find_project_config(
  const std::filesystem::path & start_dir);

/**
 * Default name of the project configuration file.
 */
inline constexpr const char * k_project_config_file_name = "btc.yaml";

}  // namespace bt_dsl
