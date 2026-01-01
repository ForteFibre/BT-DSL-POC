// test_cli_imports.cpp - CLI integration tests for import rules

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace
{

std::string read_all(const fs::path & p)
{
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_all(const fs::path & p, const std::string & s)
{
  std::ofstream out(p);
  ASSERT_TRUE(out.is_open()) << "Failed to open file for writing: " << p.string();
  out << s;
}

fs::path make_temp_dir(std::string_view prefix)
{
  const auto base = fs::temp_directory_path();
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = base / (std::string(prefix) + "_" + std::to_string(now));
  fs::create_directories(dir);
  return dir;
}

std::string shell_quote(const std::string & s)
{
  // POSIX shell single-quote escaping.
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

int run_cli_check(const fs::path & file)
{
#ifndef BT_DSL_CLI_PATH
  (void)file;
  return 0;
#else
  const std::string cli = BT_DSL_CLI_PATH;
  const std::string cmd =
    shell_quote(cli) + " check " + shell_quote(file.string()) + " > /dev/null 2>&1";

  const int rc = std::system(cmd.c_str());

#if defined(__unix__) || defined(__APPLE__)
  if (rc == -1) {
    return 127;
  }
  if (WIFEXITED(rc)) {
    return WEXITSTATUS(rc);
  }
  return 128;
#else
  // Best-effort fallback.
  return rc;
#endif
#endif
}

}  // namespace

TEST(CliImportRulesTest, ErrorsOnAbsoluteImportPath)
{
#ifndef BT_DSL_CLI_PATH
  GTEST_SKIP() << "BT_DSL_CLI_PATH is not configured (bt_dsl_cli target missing?)";
#endif
  const fs::path dir = make_temp_dir("bt_dsl_cli_import_abs");
  const fs::path main = dir / "main.bt";

  write_all(
    main,
    R"(
import "/abs.bt"
extern control Sequence();

tree Main() {
  Sequence {}
}
)");

  EXPECT_NE(run_cli_check(main), 0);
}

TEST(CliImportRulesTest, ErrorsOnImportMissingExtension)
{
#ifndef BT_DSL_CLI_PATH
  GTEST_SKIP() << "BT_DSL_CLI_PATH is not configured (bt_dsl_cli target missing?)";
#endif
  const fs::path dir = make_temp_dir("bt_dsl_cli_import_noext");
  const fs::path main = dir / "main.bt";

  write_all(
    main,
    R"(
import "./dep"
extern control Sequence();

tree Main() {
  Sequence {}
}
)");

  EXPECT_NE(run_cli_check(main), 0);
}

TEST(CliImportRulesTest, ErrorsOnPackageImport)
{
#ifndef BT_DSL_CLI_PATH
  GTEST_SKIP() << "BT_DSL_CLI_PATH is not configured (bt_dsl_cli target missing?)";
#endif
  const fs::path dir = make_temp_dir("bt_dsl_cli_import_pkg");
  const fs::path main = dir / "main.bt";

  write_all(
    main,
    R"(
import "SomeLib.bt"
extern control Sequence();

tree Main() {
  Sequence {}
}
)");

  EXPECT_NE(run_cli_check(main), 0);
}

TEST(CliImportRulesTest, OkOnDirectRelativeImport)
{
#ifndef BT_DSL_CLI_PATH
  GTEST_SKIP() << "BT_DSL_CLI_PATH is not configured (bt_dsl_cli target missing?)";
#endif
  const fs::path dir = make_temp_dir("bt_dsl_cli_import_ok");
  const fs::path dep = dir / "dep.bt";
  const fs::path main = dir / "main.bt";

  write_all(
    dep,
    R"(
extern action FromDep();
)");

  write_all(
    main,
    R"(
import "./dep.bt"
extern control Sequence();

tree Main() {
  Sequence {}
}
)");

  EXPECT_EQ(run_cli_check(main), 0);
}
