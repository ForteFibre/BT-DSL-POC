// btc - BT-DSL Compiler Command Line Interface
//
// Usage:
//   btc build [file.bt | --project] [-o output]
//   btc check [file.bt | --project]
//   btc init <project-name>
//   btc model-convert <file.xml> [-o output.bt]
//
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

#include "bt_dsl/basic/diagnostic_printer.hpp"
#include "bt_dsl/codegen/model_converter.hpp"
#include "bt_dsl/driver/compiler.hpp"
#include "bt_dsl/project/project_config.hpp"

namespace fs = std::filesystem;

namespace
{

// ============================================================================
// Output Formatting
// ============================================================================

void print_usage(const char * program_name)
{
  std::cerr << "BT-DSL Compiler v0.1.0\n\n"
            << "Usage: " << program_name << " <command> [options]\n\n"
            << "Commands:\n"
            << "  build [file.bt]          Build a file or project\n"
            << "  check [file.bt]          Check syntax and semantics (no codegen)\n"
            << "  init <project-name>      Initialize a new project\n"
            << "  model-convert <file.xml> Convert XML to BT-DSL\n\n"
            << "Options:\n"
            << "  -o, --output <path>      Output directory or file\n"
            << "  --project                Build project from btc.yaml\n"
            << "  --pkg <path>             Register package (folder name = pkg name, repeatable)\n"
            << "  --no-stdlib              Disable automatic stdlib detection\n"
            << "  -v, --verbose            Verbose output\n"
            << "  -h, --help               Show this help message\n";
}

void print_diagnostics(
  const bt_dsl::DiagnosticBag & diagnostics, const bt_dsl::ModuleGraph * graph,
  const std::string & default_filename)
{
  // Detect if terminal supports colors (simple check for TTY)
  const bool use_color = isatty(fileno(stderr)) != 0;
  bt_dsl::DiagnosticPrinter printer(std::cerr, use_color);

  for (const auto & diag : diagnostics) {
    // Try to find source context from module graph
    if (graph && !graph->empty()) {
      // For now, use the first module's source manager as default
      // In a more complete implementation, we'd track which file each diagnostic belongs to
      const auto modules = graph->get_all_modules();
      for (const auto * module : modules) {
        if (module->parsedUnit) {
          printer.print(diag, module->parsedUnit->source, module->absolutePath.string());
          break;
        }
      }
    } else {
      // Fallback: print with minimal formatting (no source context)
      std::cerr << default_filename;
      if (diag.range.is_valid()) {
        std::cerr << ":0:0";  // No line info available
      }
      std::cerr << ": ";

      const char * severity_str = "error";
      switch (diag.severity) {
        case bt_dsl::Severity::Warning:
          severity_str = "warning";
          break;
        case bt_dsl::Severity::Info:
          severity_str = "info";
          break;
        case bt_dsl::Severity::Hint:
          severity_str = "hint";
          break;
        default:
          break;
      }

      std::cerr << severity_str << ": " << diag.message;
      if (!diag.code.empty()) {
        std::cerr << " [" << diag.code << "]";
      }
      std::cerr << "\n";
    }
  }
}

// ============================================================================
// Argument Parsing
// ============================================================================

struct CommandArgs
{
  std::string command;
  std::string input_file;
  std::string output_path;
  std::vector<std::string> pkg_paths;
  bool use_project = false;
  bool no_stdlib = false;
  bool verbose = false;
  bool show_help = false;
};

CommandArgs parse_args(int argc, char * argv[])
{
  CommandArgs args;

  if (argc < 2) {
    args.show_help = true;
    return args;
  }

  args.command = argv[1];

  if (args.command == "-h" || args.command == "--help") {
    args.show_help = true;
    return args;
  }

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-o" || arg == "--output") {
      if (i + 1 < argc) {
        args.output_path = argv[++i];
      }
    } else if (arg == "--project") {
      args.use_project = true;
    } else if (arg == "--pkg") {
      if (i + 1 < argc) {
        args.pkg_paths.emplace_back(argv[++i]);
      }
    } else if (arg == "--no-stdlib") {
      args.no_stdlib = true;
    } else if (arg == "-v" || arg == "--verbose") {
      args.verbose = true;
    } else if (arg == "-h" || arg == "--help") {
      args.show_help = true;
    } else if (arg[0] != '-' && args.input_file.empty()) {
      args.input_file = arg;
    }
  }

  return args;
}

// ============================================================================
// Commands
// ============================================================================

int cmd_build(const CommandArgs & args)
{
  bt_dsl::CompileOptions options;
  options.mode = bt_dsl::CompileMode::Build;
  options.verbose = args.verbose;
  options.auto_detect_stdlib = !args.no_stdlib;
  if (!args.output_path.empty()) {
    options.output_dir = args.output_path;
  }

  // Register user-specified packages
  for (const auto & path : args.pkg_paths) {
    options.pkg_paths.emplace_back(path);
  }

  bt_dsl::CompileResult result;

  if (args.use_project || args.input_file.empty()) {
    // Project mode: find btc.yaml
    auto config_path = bt_dsl::find_project_config(fs::current_path());
    if (!config_path) {
      std::cerr << "error: no btc.yaml found in current directory or parents\n";
      return 1;
    }

    const auto config_result = bt_dsl::load_project_config(*config_path);
    if (!config_result.success) {
      std::cerr << "error: " << config_result.error << "\n";
      return 1;
    }

    if (args.verbose) {
      std::cerr << "Building project: " << config_result.config.package.name << "\n";
    }

    result = bt_dsl::Compiler::compile_project(config_result.config, options);
  } else {
    // Single file mode
    const fs::path input_path = fs::absolute(args.input_file);

    if (!fs::exists(input_path)) {
      std::cerr << "error: file not found: " << input_path.string() << "\n";
      return 1;
    }

    if (args.verbose) {
      std::cerr << "Building: " << input_path.string() << "\n";
    }

    result = bt_dsl::Compiler::compile_single_file(input_path, options);
  }

  if (!result.diagnostics.empty()) {
    print_diagnostics(
      result.diagnostics, result.module_graph.get(),
      args.input_file.empty() ? "project" : args.input_file);
  }

  if (!result.success) {
    return 1;
  }

  for (const auto & file : result.generated_files) {
    std::cerr << "Generated: " << file.string() << "\n";
  }

  return 0;
}

int cmd_check(const CommandArgs & args)
{
  bt_dsl::CompileOptions options;
  options.mode = bt_dsl::CompileMode::Check;
  options.verbose = args.verbose;
  options.auto_detect_stdlib = !args.no_stdlib;

  // Register user-specified packages
  for (const auto & path : args.pkg_paths) {
    options.pkg_paths.emplace_back(path);
  }

  bt_dsl::CompileResult result;

  if (args.use_project || args.input_file.empty()) {
    // Project mode
    auto config_path = bt_dsl::find_project_config(fs::current_path());
    if (!config_path) {
      std::cerr << "error: no btc.yaml found in current directory or parents\n";
      return 1;
    }

    const auto config_result = bt_dsl::load_project_config(*config_path);
    if (!config_result.success) {
      std::cerr << "error: " << config_result.error << "\n";
      return 1;
    }

    if (args.verbose) {
      std::cerr << "Checking project: " << config_result.config.package.name << "\n";
    }

    result = bt_dsl::Compiler::compile_project(config_result.config, options);
  } else {
    // Single file mode
    const fs::path input_path = fs::absolute(args.input_file);

    if (!fs::exists(input_path)) {
      std::cerr << "error: file not found: " << input_path.string() << "\n";
      return 1;
    }

    if (args.verbose) {
      std::cerr << "Checking: " << input_path.string() << "\n";
    }

    result = bt_dsl::Compiler::compile_single_file(input_path, options);
  }

  if (!result.diagnostics.empty()) {
    print_diagnostics(
      result.diagnostics, result.module_graph.get(),
      args.input_file.empty() ? "project" : args.input_file);
  }

  if (result.success) {
    std::cout << (args.input_file.empty() ? "project" : args.input_file) << ": OK\n";
    return 0;
  }

  return 1;
}

int cmd_init(const CommandArgs & args)
{
  if (args.input_file.empty()) {
    std::cerr << "error: project name required\n";
    std::cerr << "usage: btc init <project-name>\n";
    return 1;
  }

  const fs::path project_dir = fs::current_path() / args.input_file;

  if (fs::exists(project_dir)) {
    std::cerr << "error: directory already exists: " << project_dir.string() << "\n";
    return 1;
  }

  try {
    fs::create_directories(project_dir);
    fs::create_directories(project_dir / "src");
    fs::create_directories(project_dir / "generated");

    // Create btc.yaml
    std::ofstream config(project_dir / "btc.yaml");
    config << "package:\n"
           << "  name: '" << args.input_file << "'\n"
           << "  version: '0.1.0'\n\n"
           << "compiler:\n"
           << "  entry_points:\n"
           << "    - './src/main.bt'\n"
           << "  output_dir: './generated'\n"
           << "  target: 'btcpp_v4'\n";
    config.close();

    // Create main.bt
    std::ofstream main(project_dir / "src" / "main.bt");
    main << "/// Main behavior tree\n"
         << "tree main() {\n"
         << "  // Add your behavior tree logic here\n"
         << "  AlwaysSuccess()\n"
         << "}\n";
    main.close();

    std::cout << "Initialized new BT-DSL project in " << project_dir.string() << "\n";
    std::cout << "\nNext steps:\n"
              << "  cd " << args.input_file << "\n"
              << "  btc build\n";

    return 0;
  } catch (const std::exception & e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

int cmd_model_convert(const CommandArgs & args)
{
  if (args.input_file.empty()) {
    std::cerr << "error: input XML file required\n";
    std::cerr << "usage: btc model-convert <file.xml> [-o output.bt]\n";
    return 1;
  }

  const fs::path input_path = fs::absolute(args.input_file);

  if (!fs::exists(input_path)) {
    std::cerr << "error: file not found: " << input_path.string() << "\n";
    return 1;
  }

  // Read XML file
  std::ifstream file(input_path);
  if (!file.is_open()) {
    std::cerr << "error: failed to open file: " << input_path.string() << "\n";
    return 1;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string xml_content = buffer.str();

  try {
    auto result = bt_dsl::ModelConverter::convert(xml_content);

    if (args.output_path.empty()) {
      // Output to stdout
      std::cout << result.bt_text;
    } else {
      // Write to file
      std::ofstream out(args.output_path);
      if (!out.is_open()) {
        std::cerr << "error: failed to open output file: " << args.output_path << "\n";
        return 1;
      }
      out << result.bt_text;
      std::cerr << "Converted " << result.nodes_count << " nodes to " << args.output_path << "\n";
    }

    return 0;
  } catch (const std::exception & e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

}  // namespace

int main(int argc, char * argv[])
{
  const CommandArgs args = parse_args(argc, argv);

  if (args.show_help) {
    print_usage(argv[0]);
    return 0;
  }

  if (args.command == "build") {
    return cmd_build(args);
  }

  if (args.command == "check") {
    return cmd_check(args);
  }

  if (args.command == "init") {
    return cmd_init(args);
  }

  if (args.command == "model-convert") {
    return cmd_model_convert(args);
  }

  std::cerr << "error: unknown command '" << args.command << "'\n";
  print_usage(argv[0]);
  return 1;
}
