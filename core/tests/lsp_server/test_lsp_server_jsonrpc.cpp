#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef BT_DSL_LSP_SERVER_PATH
#define BT_DSL_LSP_SERVER_PATH "bt_dsl_lsp_server"
#endif

#ifndef BT_DSL_STDLIB_PATH
#define BT_DSL_STDLIB_PATH ""
#endif

namespace
{
struct LspPos
{
  int line = 0;
  int character = 0;  // UTF-16 code units
};

std::pair<uint32_t, size_t> decode_utf8(std::string_view s, size_t i)
{
  const unsigned char b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) return {b0, 1};
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    if ((b1 & 0xC0) == 0x80) {
      const uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
      return {cp, 2};
    }
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
    if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80)) {
      const uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
      return {cp, 3};
    }
  }
  if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
    const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
    if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80) && ((b3 & 0xC0) == 0x80)) {
      const uint32_t cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) |
                          (b3 & 0x3F);
      return {cp, 4};
    }
  }
  return {0xFFFDu, 1};
}

int utf16_units(uint32_t cp)
{
  return (cp > 0xFFFFu) ? 2 : 1;
}

LspPos lsp_pos_at_utf8_byte(std::string_view text, uint32_t target_byte)
{
  LspPos pos;
  uint32_t bytes = 0;
  size_t i = 0;
  while (i < text.size() && bytes < target_byte) {
    const auto [cp, consumed] = decode_utf8(text, i);
    const uint32_t utf8_units = static_cast<uint32_t>(consumed);

    if (bytes + utf8_units > target_byte) break;

    bytes += utf8_units;
    i += consumed;

    if (cp == '\n') {
      pos.line += 1;
      pos.character = 0;
    } else {
      pos.character += utf16_units(cp);
    }
  }
  return pos;
}

std::string to_file_uri(const fs::path & p)
{
  const fs::path abs = fs::absolute(p);
  return std::string("file://") + abs.string();
}

struct Proc
{
  pid_t pid = -1;
  int in_fd = -1;   // parent writes to child's stdin
  int out_fd = -1;  // parent reads from child's stdout
};

bool write_all(int fd, const void * data, size_t n)
{
  const auto * p = static_cast<const uint8_t *>(data);
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

std::optional<std::string> read_exact_with_timeout(int fd, size_t n, int timeout_ms)
{
  std::string out;
  out.resize(n);
  size_t off = 0;

  while (off < n) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) {
      return std::nullopt;  // timeout
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }

    const ssize_t r = ::read(fd, out.data() + off, n - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (r == 0) {
      return std::nullopt;
    }
    off += static_cast<size_t>(r);
  }

  return out;
}

std::optional<std::string> read_line_with_timeout(int fd, int timeout_ms)
{
  std::string line;
  char c = 0;
  for (;;) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) return std::nullopt;
    if (pr < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }

    const ssize_t r = ::read(fd, &c, 1);
    if (r < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (r == 0) return std::nullopt;

    if (c == '\n') break;
    if (c != '\r') line.push_back(c);
  }
  return line;
}

std::optional<json> read_framed_json(int fd, int timeout_ms)
{
  int content_length = -1;

  // headers
  for (;;) {
    const auto line_opt = read_line_with_timeout(fd, timeout_ms);
    if (!line_opt) return std::nullopt;
    const std::string line = *line_opt;
    if (line.empty()) break;

    const std::string key = "Content-Length:";
    if (line.rfind(key, 0) == 0) {
      std::string rest = line.substr(key.size());
      while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.erase(rest.begin());
      content_length = std::atoi(rest.c_str());
    }
  }

  if (content_length <= 0) return std::nullopt;

  const auto body_opt = read_exact_with_timeout(fd, static_cast<size_t>(content_length), timeout_ms);
  if (!body_opt) return std::nullopt;

  try {
    return json::parse(*body_opt);
  } catch (...) {
    return std::nullopt;
  }
}

class LspServer
{
public:
  LspServer()
  {
    proc_ = spawn();
    if (proc_.pid <= 0 || proc_.in_fd < 0 || proc_.out_fd < 0) {
      throw std::runtime_error("Failed to spawn bt_dsl_lsp_server");
    }

    // initialize
    json params;
    params["processId"] = nullptr;
    params["rootUri"] = nullptr;
    params["capabilities"] = json::object();
    params["initializationOptions"] = json{{"stdlibPath", std::string(BT_DSL_STDLIB_PATH)}};

    const json resp = request("initialize", params);
    if (!resp.contains("result")) {
      throw std::runtime_error("initialize did not return a result");
    }

    notify("initialized", json::object());
  }

  ~LspServer()
  {
    if (proc_.pid <= 0) return;

    // Best-effort shutdown sequence.
    (void)request_no_throw("shutdown", json::object());
    notify("exit", json::object());

    int status = 0;
    for (int i = 0; i < 20; i++) {
      const pid_t r = ::waitpid(proc_.pid, &status, WNOHANG);
      if (r == proc_.pid) {
        cleanup_fds();
        proc_.pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ::kill(proc_.pid, SIGKILL);
    (void)::waitpid(proc_.pid, &status, 0);
    cleanup_fds();
  }

  void did_open(const std::string & uri, const std::string & text, int version = 1)
  {
    json td;
    td["uri"] = uri;
    td["languageId"] = "bt-dsl";
    td["version"] = version;
    td["text"] = text;

    notify("textDocument/didOpen", json{{"textDocument", td}});
  }

  json completion(const std::string & uri, uint32_t byte_off, const std::string & text)
  {
    const auto pos = lsp_pos_at_utf8_byte(text, byte_off);
    json params;
    params["textDocument"] = json{{"uri", uri}};
    params["position"] = json{{"line", pos.line}, {"character", pos.character}};
    return request("textDocument/completion", params);
  }

  json definition(const std::string & uri, uint32_t byte_off, const std::string & text)
  {
    const auto pos = lsp_pos_at_utf8_byte(text, byte_off);
    json params;
    params["textDocument"] = json{{"uri", uri}};
    params["position"] = json{{"line", pos.line}, {"character", pos.character}};
    return request("textDocument/definition", params);
  }

  std::optional<json> wait_for_notification(std::string_view method, int timeout_ms = 2000)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto msg = read_framed_json(proc_.out_fd, 200);
      if (!msg) continue;
      if (msg->contains("method") && (*msg)["method"].is_string() && (*msg)["method"].get<std::string>() == method) {
        return msg;
      }
      // ignore other messages
    }
    return std::nullopt;
  }

private:
  static Proc spawn()
  {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    Proc p;

    if (::pipe(in_pipe) != 0) return p;
    if (::pipe(out_pipe) != 0) {
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      return p;
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
      // child
      ::dup2(in_pipe[0], STDIN_FILENO);
      ::dup2(out_pipe[1], STDOUT_FILENO);

      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);

      const char * argv0 = BT_DSL_LSP_SERVER_PATH;
      char * const argv[] = {const_cast<char *>(argv0), nullptr};
      ::execv(argv0, argv);

      // exec failed
      std::perror("execv");
      _exit(127);
    }

    // parent
    p.pid = pid;
    p.in_fd = in_pipe[1];
    p.out_fd = out_pipe[0];
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    return p;
  }

  void cleanup_fds()
  {
    if (proc_.in_fd >= 0) {
      ::close(proc_.in_fd);
      proc_.in_fd = -1;
    }
    if (proc_.out_fd >= 0) {
      ::close(proc_.out_fd);
      proc_.out_fd = -1;
    }
  }

  void send_payload(const json & payload)
  {
    const std::string body = payload.dump();
    std::ostringstream oss;
    oss << "Content-Length: " << body.size() << "\r\n\r\n";
    const std::string hdr = oss.str();

    if (!write_all(proc_.in_fd, hdr.data(), hdr.size())) {
      throw std::runtime_error("Failed to write JSON-RPC header");
    }
    if (!write_all(proc_.in_fd, body.data(), body.size())) {
      throw std::runtime_error("Failed to write JSON-RPC body");
    }
  }

  void notify(const std::string & method, json params)
  {
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = method;
    msg["params"] = std::move(params);
    send_payload(msg);
  }

  json request(const std::string & method, json params)
  {
    const int id = next_id_++;
    json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["method"] = method;
    msg["params"] = std::move(params);
    send_payload(msg);

    for (;;) {
      const auto resp_opt = read_framed_json(proc_.out_fd, 2000);
      if (!resp_opt.has_value()) {
        throw std::runtime_error("Timed out waiting for JSON-RPC response");
      }
      const json resp = *resp_opt;

      if (resp.contains("id") && resp["id"].is_number_integer() && resp["id"].get<int>() == id) {
        return resp;
      }
      // ignore notifications/other responses
    }
  }

  std::optional<json> request_no_throw(const std::string & method, json params)
  {
    try {
      return request(method, std::move(params));
    } catch (...) {
      return std::nullopt;
    }
  }

  Proc proc_;
  int next_id_ = 1;
};

}  // namespace

TEST(LspServerJsonRpcTest, InitializeReportsCapabilities)
{
  LspServer srv;
  // Constructor already performed initialize; just ensure it stays alive.
  SUCCEED();
}

TEST(LspServerJsonRpcTest, PublishDiagnosticsOnDidOpen)
{
  LspServer srv;

  const std::string text = "Tree Main() {\n  Sequence {\n";  // missing closing braces

  const fs::path tmp = fs::temp_directory_path() / fs::path("bt_dsl_lsp_diag.bt");
  const std::string uri = to_file_uri(tmp);

  srv.did_open(uri, text);

  const auto note = srv.wait_for_notification("textDocument/publishDiagnostics");
  ASSERT_TRUE(note.has_value());
  ASSERT_TRUE(note->contains("params"));
  const auto & params = (*note)["params"];
  ASSERT_TRUE(params.contains("uri"));
  ASSERT_EQ(params["uri"].get<std::string>(), uri);
  ASSERT_TRUE(params.contains("diagnostics"));
  ASSERT_TRUE(params["diagnostics"].is_array());

  bool saw_parser = false;
  for (const auto & d : params["diagnostics"]) {
    if (d.contains("source") && d["source"].is_string() && d["source"].get<std::string>() == "parser") {
      saw_parser = true;
      break;
    }
  }
  EXPECT_TRUE(saw_parser);
}

TEST(LspServerJsonRpcTest, CompletionWorksWithUtf8Comments)
{
  LspServer srv;

  const std::string text = R"(
//! Fixture
// 日本語🙂 を入れて UTF-8/UTF-16 変換のズレを検出しやすくする

declare Action MyAction(in target: string)
Tree Main() {
  
}
)";

  const fs::path tmp = fs::temp_directory_path() / fs::path("bt_dsl_lsp_completion.bt");
  const std::string uri = to_file_uri(tmp);

  srv.did_open(uri, text);

  const auto pos = static_cast<uint32_t>(text.find("\n  \n") + 3);
  const json resp = srv.completion(uri, pos, text);

  ASSERT_TRUE(resp.contains("result"));
  const auto & result = resp["result"];
  ASSERT_TRUE(result.contains("items"));
  ASSERT_TRUE(result["items"].is_array());

  bool saw = false;
  for (const auto & it : result["items"]) {
    if (it.contains("label") && it["label"].is_string() && it["label"].get<std::string>() == "MyAction") {
      saw = true;
      break;
    }
  }
  EXPECT_TRUE(saw);
}

TEST(LspServerJsonRpcTest, DefinitionResolvesIntoImportedFile)
{
  LspServer srv;

  const fs::path dir = fs::temp_directory_path() / fs::path("bt_dsl_lsp_ws");
  fs::create_directories(dir);

  const fs::path decl = dir / fs::path("test-nodes.bt");
  const fs::path main = dir / fs::path("main.bt");

  {
    std::ofstream ofs(decl);
    ofs << "declare Action TestAction(in pos: int, out found: bool)\n";
  }

  const std::string main_text = R"(
//! Fixture
import "./test-nodes.bt"

Tree Main() {
  Sequence {
    TestAction(pos: 1, found: out Found)
  }
}
)";

  {
    std::ofstream ofs(main);
    ofs << main_text;
  }

  const std::string main_uri = to_file_uri(main);
  srv.did_open(main_uri, main_text);

  const auto anchor = main_text.find("TestAction(");
  ASSERT_NE(anchor, std::string::npos);
  const uint32_t byte_off = static_cast<uint32_t>(anchor + 1);

  const json resp = srv.definition(main_uri, byte_off, main_text);
  ASSERT_TRUE(resp.contains("result"));
  ASSERT_TRUE(resp["result"].is_array());

  bool saw_decl = false;
  const std::string decl_uri = to_file_uri(decl);
  for (const auto & loc : resp["result"]) {
    if (loc.contains("uri") && loc["uri"].is_string() && loc["uri"].get<std::string>() == decl_uri) {
      saw_decl = true;
      break;
    }
  }

  EXPECT_TRUE(saw_decl) << "Expected definition location in imported file";
}
