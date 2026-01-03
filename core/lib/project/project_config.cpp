// bt_dsl/project/project_config.cpp - Project configuration implementation
//
#include "bt_dsl/project/project_config.hpp"

#include <yaml-cpp/yaml.h>

namespace bt_dsl
{

namespace
{

/// Parse a single dependency entry
std::optional<DependencyConfig> parse_dependency(const YAML::Node & node, std::string & error)
{
  if (!node.IsMap()) {
    error = "dependency entry must be a map";
    return std::nullopt;
  }

  DependencyConfig dep;

  if (node["path"]) {
    dep.path = node["path"].as<std::string>();
  }

  if (node["ros_package"]) {
    dep.ros_package = node["ros_package"].as<std::string>();
  }

  if (!dep.path && !dep.ros_package) {
    error = "dependency must have either 'path' or 'ros_package'";
    return std::nullopt;
  }

  return dep;
}

}  // namespace

ConfigLoadResult load_project_config(const std::filesystem::path & config_path)
{
  namespace fs = std::filesystem;

  // Check if file exists
  if (!fs::exists(config_path)) {
    return ConfigLoadResult::fail("configuration file not found: " + config_path.string());
  }

  // Load YAML
  YAML::Node root;
  try {
    root = YAML::LoadFile(config_path.string());
  } catch (const YAML::Exception & e) {
    return ConfigLoadResult::fail("failed to parse YAML: " + std::string(e.what()));
  }

  ProjectConfig config;
  config.project_root = fs::absolute(config_path).parent_path();

  // Parse 'package' section
  if (root["package"]) {
    const auto & pkg = root["package"];
    if (pkg["name"]) {
      config.package.name = pkg["name"].as<std::string>();
    }
    if (pkg["version"]) {
      config.package.version = pkg["version"].as<std::string>();
    }
  }

  // Parse 'compiler' section
  if (root["compiler"]) {
    const auto & comp = root["compiler"];

    if (comp["entry_points"]) {
      if (!comp["entry_points"].IsSequence()) {
        return ConfigLoadResult::fail("compiler.entry_points must be a list");
      }
      for (const auto & ep : comp["entry_points"]) {
        config.compiler.entry_points.emplace_back(ep.as<std::string>());
      }
    }

    if (comp["output_dir"]) {
      config.compiler.output_dir = comp["output_dir"].as<std::string>();
    }

    if (comp["target"]) {
      config.compiler.target = comp["target"].as<std::string>();
      // Validate target
      if (config.compiler.target != "btcpp_v4" && config.compiler.target != "btcpp_v4_strict") {
        return ConfigLoadResult::fail(
          "invalid compiler.target: '" + config.compiler.target +
          "' (must be 'btcpp_v4' or 'btcpp_v4_strict')");
      }
    }
  }

  // Parse 'dependencies' section
  if (root["dependencies"]) {
    if (!root["dependencies"].IsSequence()) {
      return ConfigLoadResult::fail("dependencies must be a list");
    }
    for (const auto & dep_node : root["dependencies"]) {
      std::string dep_error;
      auto dep = parse_dependency(dep_node, dep_error);
      if (!dep) {
        return ConfigLoadResult::fail("invalid dependency: " + dep_error);
      }
      config.dependencies.push_back(std::move(*dep));
    }
  }

  return ConfigLoadResult::ok(std::move(config));
}

std::optional<std::filesystem::path> find_project_config(const std::filesystem::path & start_dir)
{
  namespace fs = std::filesystem;

  fs::path current = fs::absolute(start_dir);

  // If startDir is a file, start from its parent
  if (fs::is_regular_file(current)) {
    current = current.parent_path();
  }

  while (true) {
    fs::path candidate = current / k_project_config_file_name;
    if (fs::exists(candidate)) {
      return candidate;
    }

    // Move up to parent
    const fs::path parent = current.parent_path();
    if (parent == current) {
      // Reached filesystem root
      break;
    }
    current = parent;
  }

  return std::nullopt;
}

}  // namespace bt_dsl
