// bt-dsl-cli - Command line interface for BT-DSL
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/parser/parser.hpp"
#include "bt_dsl/semantic/analyzer.hpp"
#include "manifest_converter.hpp"

namespace
{

// Forward declarations (used by import-loading helpers).
std::string read_file(const std::string & path);
void print_parse_errors(
  const std::vector<bt_dsl::ParseError> & errors, const std::string & filename);

bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool is_relative_import_spec(std::string_view spec)
{
  return starts_with(spec, "./") || starts_with(spec, "../");
}

bool has_required_extension(std::string_view spec)
{
  // Reference: docs/reference/declarations-and-scopes.md 4.1.3
  // Extension is required (e.g. "./foo.bt" is ok, "./foo" is not).
  const std::filesystem::path p{std::string(spec)};
  return !p.extension().empty();
}

struct ImportLoadResult
{
  // Direct imports of the main program (non-transitive visibility).
  std::vector<const bt_dsl::Program *> main_direct_imports;
  // Direct-import graph for all loaded modules (including main).
  bt_dsl::ImportGraph direct_imports;
};

std::optional<ImportLoadResult> load_transitive_imports(
  bt_dsl::Parser & parser, const bt_dsl::Program & main_program, const std::string & input_file,
  std::vector<std::unique_ptr<bt_dsl::Program>> & owned_import_programs,
  bt_dsl::DiagnosticBag & diagnostics)
{
  namespace fs = std::filesystem;

  const fs::path main_path = fs::absolute(fs::path(input_file));

  // Map absolute path -> Program*
  std::unordered_map<std::string, const bt_dsl::Program *> by_path;
  by_path.reserve(32);

  ImportLoadResult out;
  out.direct_imports.reserve(32);

  // Keep a separate set to preserve "ignore duplicates" behavior per spec.
  std::unordered_set<std::string> seen;
  seen.reserve(64);

  std::function<const bt_dsl::Program *(const fs::path &, const bt_dsl::ImportStmt &)> load_one;
  load_one =
    [&](const fs::path & from_file, const bt_dsl::ImportStmt & imp) -> const bt_dsl::Program * {
    const std::string_view spec = imp.path;

    // Enforce reference constraints for relative paths (package imports are
    // implementation-defined; this CLI currently supports only relative ones).
    if (starts_with(spec, "/")) {
      diagnostics.error(imp.range, "Absolute import paths are not allowed: \"" + imp.path + "\"");
      return nullptr;
    }
    if (!has_required_extension(spec)) {
      diagnostics.error(imp.range, "Import path must include an extension: \"" + imp.path + "\"");
      return nullptr;
    }
    if (!is_relative_import_spec(spec)) {
      diagnostics.error(imp.range, "Cannot resolve package import in CLI: \"" + imp.path + "\"");
      return nullptr;
    }

    const fs::path base_dir = fs::absolute(from_file).parent_path();
    const fs::path resolved = (base_dir / fs::path(std::string(spec))).lexically_normal();
    const std::string resolved_str = resolved.string();

    // Ignore duplicates across the entire import closure (best-effort).
    if (!seen.insert(resolved_str).second) {
      auto it = by_path.find(resolved_str);
      return (it != by_path.end()) ? it->second : nullptr;
    }

    // Already loaded?
    if (auto it = by_path.find(resolved_str); it != by_path.end()) {
      return it->second;
    }

    std::string imported_source;
    try {
      imported_source = read_file(resolved_str);
    } catch (const std::exception & e) {
      diagnostics.error(imp.range, std::string("Failed to read imported file: ") + e.what());
      return nullptr;
    }

    auto parsed = parser.parse(imported_source);
    if (!parsed) {
      // Print parse errors for the imported file directly and stop.
      print_parse_errors(parsed.error(), resolved_str);
      return nullptr;
    }

    owned_import_programs.push_back(std::make_unique<bt_dsl::Program>(std::move(parsed.value())));
    const bt_dsl::Program * prog_ptr = owned_import_programs.back().get();
    by_path.emplace(resolved_str, prog_ptr);

    // Load this module's direct imports recursively.
    std::vector<const bt_dsl::Program *> direct;
    direct.reserve(prog_ptr->imports.size());
    for (const auto & child_imp : prog_ptr->imports) {
      if (const bt_dsl::Program * child = load_one(resolved, child_imp)) {
        direct.push_back(child);
      }
    }
    out.direct_imports.emplace(prog_ptr, std::move(direct));

    return prog_ptr;
  };

  // Load main's direct imports (visibility is non-transitive).
  std::vector<const bt_dsl::Program *> main_direct;
  main_direct.reserve(main_program.imports.size());
  for (const auto & imp : main_program.imports) {
    if (const bt_dsl::Program * p = load_one(main_path, imp)) {
      main_direct.push_back(p);
    }
  }
  out.main_direct_imports = std::move(main_direct);
  out.direct_imports.emplace(&main_program, out.main_direct_imports);

  if (!diagnostics.errors().empty()) {
    return std::nullopt;
  }
  return out;
}

const char * severity_to_cstr(bt_dsl::Severity s)
{
  switch (s) {
    case bt_dsl::Severity::Error:
      return "error";
    case bt_dsl::Severity::Warning:
      return "warning";
    case bt_dsl::Severity::Info:
      return "info";
    case bt_dsl::Severity::Hint:
      return "hint";
  }
  return "error";
}

void print_usage(const char * program_name)
{
  std::cerr << "Usage: " << program_name << " <command> [options] <file>\n"
            << "\n"
            << "Commands:\n"
            << "  check   <file.bt>              Check syntax and semantics\n"
            << "  convert <file.bt> [-o output]  Convert to BehaviorTree.CPP "
               "XML\n"
            << "  xml-to-bt <file.xml> [-o output] Convert XML manifest to BT "
               "DSL\n"
            << "\n"
            << "Options:\n"
            << "  -o, --output <file>  Output file (default: stdout)\n"
            << "  -h, --help           Show this help message\n";
}

std::string read_file(const std::string & path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void write_file(const std::string & path, const std::string & content)
{
  std::ofstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file for writing: " + path);
  }
  file << content;
}

void print_diagnostics(const bt_dsl::DiagnosticBag & diagnostics, const std::string & filename)
{
  for (const auto & diag : diagnostics) {
    const char * severity_str = severity_to_cstr(diag.severity);

    std::cerr << filename << ":" << diag.range.start_line << ":" << diag.range.start_column << ": "
              << severity_str << ": " << diag.message;
    if (!diag.code.empty()) {
      std::cerr << " [" << diag.code << "]";
    }
    std::cerr << "\n";
  }
}

void print_parse_errors(
  const std::vector<bt_dsl::ParseError> & errors, const std::string & filename)
{
  for (const auto & err : errors) {
    std::cerr << filename << ":" << err.range.start_line << ":" << err.range.start_column
              << ": error: " << err.message << "\n";
  }
}

int cmd_check(const std::string & input_file)
{
  // Read source file
  std::string source;
  try {
    source = read_file(input_file);
  } catch (const std::exception & e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  // Parse
  bt_dsl::Parser parser;
  auto parse_result = parser.parse(source);

  if (!parse_result) {
    print_parse_errors(parse_result.error(), input_file);
    return 1;
  }

  // Load transitive imports (for correct module-local analysis), but keep
  // non-transitive *visibility* by passing only main's direct imports to Analyzer.
  bt_dsl::DiagnosticBag import_diags;
  std::vector<std::unique_ptr<bt_dsl::Program>> owned_imports;
  auto imports_opt =
    load_transitive_imports(parser, parse_result.value(), input_file, owned_imports, import_diags);
  if (!imports_opt) {
    if (!import_diags.errors().empty()) {
      print_diagnostics(import_diags, input_file);
    }
    return 1;
  }
  const auto & import_ptrs = imports_opt->main_direct_imports;

  // Semantic analysis
  auto analysis = bt_dsl::Analyzer::analyze(parse_result.value(), import_ptrs);

  if (analysis.has_errors()) {
    print_diagnostics(analysis.diagnostics, input_file);
    return 1;
  }

  // Print warnings if any
  if (!analysis.diagnostics.empty()) {
    print_diagnostics(analysis.diagnostics, input_file);
  }

  std::cout << input_file << ": OK\n";
  return 0;
}

int cmd_convert(const std::string & input_file, const std::string & output_file)
{
  // Read source file
  std::string source;
  try {
    source = read_file(input_file);
  } catch (const std::exception & e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  // Parse
  bt_dsl::Parser parser;
  auto parse_result = parser.parse(source);

  if (!parse_result) {
    print_parse_errors(parse_result.error(), input_file);
    return 1;
  }

  // Load transitive imports (needed for single-output XML to include reachable imported trees).
  bt_dsl::DiagnosticBag import_diags;
  std::vector<std::unique_ptr<bt_dsl::Program>> owned_imports;
  auto imports_opt =
    load_transitive_imports(parser, parse_result.value(), input_file, owned_imports, import_diags);
  if (!imports_opt) {
    if (!import_diags.errors().empty()) {
      print_diagnostics(import_diags, input_file);
    }
    return 1;
  }
  const auto & import_ptrs = imports_opt->main_direct_imports;

  // Semantic analysis
  auto analysis = bt_dsl::Analyzer::analyze(parse_result.value(), import_ptrs);

  if (analysis.has_errors()) {
    print_diagnostics(analysis.diagnostics, input_file);
    return 1;
  }

  // Generate XML (single-output, includes reachable imported trees)
  const std::string xml =
    bt_dsl::XmlGenerator::generate(parse_result.value(), analysis, imports_opt->direct_imports);

  // Output
  if (output_file.empty()) {
    std::cout << xml;
  } else {
    try {
      write_file(output_file, xml);
      std::cerr << "Wrote: " << output_file << "\n";
    } catch (const std::exception & e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }
  }

  return 0;
}

}  // namespace

int main(int argc, char * argv[])
{
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string command = argv[1];

  // Help
  if (command == "-h" || command == "--help") {
    print_usage(argv[0]);
    return 0;
  }

  // Check command
  if (command == "check") {
    if (argc < 3) {
      std::cerr << "error: missing input file\n";
      print_usage(argv[0]);
      return 1;
    }
    return cmd_check(argv[2]);
  }

  // Convert command
  if (command == "convert") {
    if (argc < 3) {
      std::cerr << "error: missing input file\n";
      print_usage(argv[0]);
      return 1;
    }

    const std::string input_file = argv[2];
    std::string output_file;

    // Parse optional -o argument
    for (int i = 3; i < argc; ++i) {
      if (
        (std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--output") == 0) &&
        i + 1 < argc) {
        output_file = argv[++i];
      }
    }

    return cmd_convert(input_file, output_file);
  }

  // XML to BT command
  if (command == "xml-to-bt") {
    if (argc < 3) {
      std::cerr << "error: missing input file\n";
      print_usage(argv[0]);
      return 1;
    }

    const std::string input_file = argv[2];
    std::string output_file;

    // Parse optional -o argument
    for (int i = 3; i < argc; ++i) {
      if (
        (std::strcmp(argv[i], "-o") == 0 || std::strcmp(argv[i], "--output") == 0) &&
        i + 1 < argc) {
        output_file = argv[++i];
      }
    }

    std::string xml_content;
    try {
      xml_content = read_file(input_file);
    } catch (const std::exception & e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }

    try {
      auto result = bt_dsl::ManifestConverter::convert(xml_content);

      if (output_file.empty()) {
        std::cout << result.bt_text;
      } else {
        write_file(output_file, result.bt_text);
        std::cerr << "Converted " << result.nodes_count << " nodes to " << output_file << "\n";
      }
    } catch (const std::exception & e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }

    return 0;
  }

  std::cerr << "error: unknown command '" << command << "'\n";
  print_usage(argv[0]);
  return 1;
}
