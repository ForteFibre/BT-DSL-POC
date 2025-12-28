// bt-dsl-cli - Command line interface for BT-DSL
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/parser/parser.hpp"
#include "bt_dsl/semantic/analyzer.hpp"
#include "manifest_converter.hpp"

namespace
{

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

  // Semantic analysis
  auto analysis = bt_dsl::Analyzer::analyze(parse_result.value());

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

  // Semantic analysis
  auto analysis = bt_dsl::Analyzer::analyze(parse_result.value());

  if (analysis.has_errors()) {
    print_diagnostics(analysis.diagnostics, input_file);
    return 1;
  }

  // Generate XML
  const std::string xml = bt_dsl::XmlGenerator::generate(parse_result.value(), analysis);

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
