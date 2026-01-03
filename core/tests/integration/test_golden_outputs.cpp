#include <gtest/gtest.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/ast/ast_dumper.hpp"
#include "bt_dsl/driver/compiler.hpp"
#include "tinyxml2.h"

using namespace bt_dsl;

namespace
{

std::string read_file(const std::filesystem::path & p)
{
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("failed to open file: " + p.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const std::filesystem::path & p, const std::string & content)
{
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  if (!out.is_open()) {
    throw std::runtime_error("failed to write file: " + p.string());
  }
  out << content;
}

std::string normalize_text(std::string s)
{
  // Normalize CRLF/CR to LF
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') {
        ++i;
      }
      out.push_back('\n');
      continue;
    }
    out.push_back(c);
  }

  // Trim trailing whitespace per line (stable across formatters)
  std::string out2;
  out2.reserve(out.size());
  std::string line;
  line.reserve(256);
  for (const char c : out) {
    if (c == '\n') {
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }
      out2 += line;
      out2.push_back('\n');
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    out2 += line;
  }

  if (!out2.empty() && out2.back() != '\n') {
    out2.push_back('\n');
  }
  return out2;
}

std::string canonicalize_xml(std::string xml)
{
  // First normalize line endings and trailing whitespace to make parsing stable.
  xml = normalize_text(std::move(xml));

  tinyxml2::XMLDocument doc;
  doc.Parse(xml.c_str());
  if (doc.Error()) {
    // If parsing fails, fall back to textual normalization so the diff hint still works.
    return xml;
  }

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);

  return normalize_text(std::string(printer.CStr()));
}

void fail_diff_hint(
  std::string_view label, const std::string & expected, const std::string & actual)
{
  const size_t n = std::min(expected.size(), actual.size());
  size_t pos = 0;
  for (; pos < n; ++pos) {
    if (expected[pos] != actual[pos]) break;
  }

  std::cerr << "Mismatch in " << label << " at byte " << pos << "\n";
  const size_t start = (pos > 80) ? (pos - 80) : 0;
  const size_t end = std::min(pos + 200, n);
  std::cerr << "--- expected (snippet) ---\n";
  std::cerr << expected.substr(start, end - start) << "\n";
  std::cerr << "--- actual (snippet) ---\n";
  std::cerr << actual.substr(start, end - start) << "\n";
}

std::string dump_program_ast(const ModuleInfo & m)
{
  std::ostringstream ss;
  AstDumper dumper(ss);
  dumper.dump(m.program);
  return ss.str();
}

std::filesystem::path get_this_dir() { return std::filesystem::absolute(__FILE__).parent_path(); }

std::filesystem::path get_core_dir()
{
  // .../core/tests/integration
  const auto integration_dir = get_this_dir();
  return integration_dir.parent_path().parent_path();
}

std::filesystem::path inputs_dir() { return get_this_dir() / "golden" / "inputs"; }
std::filesystem::path expected_dir() { return get_this_dir() / "golden" / "expected"; }

std::filesystem::path std_pkg_path() { return get_core_dir() / "std"; }

std::vector<std::filesystem::path> list_bt_files(const std::filesystem::path & dir)
{
  std::vector<std::filesystem::path> files;
  for (const auto & ent : std::filesystem::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() == ".bt") {
      files.push_back(ent.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void print_diagnostics(const DiagnosticBag & diags)
{
  for (const auto & d : diags.all()) {
    std::cerr << d.message << "\n";
  }
}

bool should_update_golden()
{
  const char * v = std::getenv("BT_DSL_UPDATE_GOLDEN");
  return v != nullptr && *v != '\0' && std::string_view(v) != "0";
}

void run_one(const std::filesystem::path & bt_file)
{
  const std::string stem = bt_file.stem().string();

  const std::filesystem::path out_dir =
    std::filesystem::temp_directory_path() / "bt_dsl_core_integration";
  std::filesystem::create_directories(out_dir);

  CompileOptions opts;
  opts.mode = CompileMode::Build;
  opts.output_dir = out_dir;

  // Register std package and local test packages (if present)
  opts.pkg_paths.push_back(std_pkg_path());

  const auto pkgs_dir = inputs_dir() / "pkgs";
  if (std::filesystem::exists(pkgs_dir)) {
    for (const auto & ent : std::filesystem::directory_iterator(pkgs_dir)) {
      if (ent.is_directory()) {
        opts.pkg_paths.push_back(ent.path());
      }
    }
  }

  const CompileResult res = Compiler::compile_single_file(bt_file, opts);
  if (!res.success) {
    std::cerr << "Compilation failed for: " << bt_file << "\n";
    print_diagnostics(res.diagnostics);
    FAIL() << "Compilation failed for: " << bt_file.string();
    return;
  }

  // Locate entry module
  ASSERT_TRUE(res.module_graph);
  ModuleInfo * entry = res.module_graph->get_module(bt_file);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(entry->program, nullptr);

  // Produced XML
  const std::filesystem::path produced_xml_path = out_dir / (stem + ".xml");
  const std::string produced_xml = canonicalize_xml(read_file(produced_xml_path));

  // Produced AST dump
  const std::string produced_ast = normalize_text(dump_program_ast(*entry));

  const std::filesystem::path exp_xml_path = expected_dir() / (stem + ".xml");
  const std::filesystem::path exp_ast_path = expected_dir() / (stem + ".ast.txt");

  if (should_update_golden()) {
    write_file(exp_xml_path, produced_xml);
    write_file(exp_ast_path, produced_ast);
    std::cerr << "[golden updated] " << stem << "\n";
    return;
  }

  const std::string expected_xml = canonicalize_xml(read_file(exp_xml_path));
  const std::string expected_ast = normalize_text(read_file(exp_ast_path));

  if (expected_xml != produced_xml) {
    std::cerr << "XML golden mismatch for: " << stem << "\n";
    fail_diff_hint("xml", expected_xml, produced_xml);
    FAIL() << "XML golden mismatch for: " << stem;
    return;
  }

  if (expected_ast != produced_ast) {
    std::cerr << "AST golden mismatch for: " << stem << "\n";
    fail_diff_hint("ast", expected_ast, produced_ast);
    FAIL() << "AST golden mismatch for: " << stem;
    return;
  }
}

}  // namespace

TEST(IntegrationGolden, MatchesGoldenOutputs)
{
  try {
    const auto dir = inputs_dir();
    ASSERT_TRUE(std::filesystem::exists(dir)) << "Inputs directory missing: " << dir;

    const auto bt_files = list_bt_files(dir);
    ASSERT_FALSE(bt_files.empty()) << "No .bt inputs found in: " << dir;

    for (const auto & f : bt_files) {
      SCOPED_TRACE(std::string("input=") + f.string());
      run_one(f);
    }
  } catch (const std::exception & e) {
    FAIL() << "Unhandled exception: " << e.what();
  }
}
