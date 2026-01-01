#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bt_dsl/lsp/lsp.hpp"

namespace
{
using json = nlohmann::json;
namespace fs = std::filesystem;

const std::vector<std::string> k_semantic_types = {
  "keyword", "class", "function", "variable", "parameter", "property", "type", "decorator"};

const std::vector<std::string> k_semantic_mods = {"declaration", "modification", "defaultLibrary"};

void log_handler_error(std::string_view method, const std::exception * e)
{
  if (e != nullptr) {
    std::cerr << "bt_dsl_lsp_server: error handling '" << method << "': " << e->what() << "\n";
    return;
  }
  std::cerr << "bt_dsl_lsp_server: error handling '" << method << "': unknown error\n";
}

// -----------------------------
// Small utilities
// -----------------------------

std::string trim_cr(std::string s)
{
  if (!s.empty() && s.back() == '\r') {
    s.pop_back();
  }
  return s;
}

bool starts_with(std::string_view s, std::string_view p)
{
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

int hex_value(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::string percent_decode(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      const int hi = hex_value(s[i + 1]);
      const int lo = hex_value(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

std::optional<fs::path> file_uri_to_path(std::string_view uri)
{
  // Typical VS Code URIs: file:///home/user/file.bt
  if (!starts_with(uri, "file://")) {
    return std::nullopt;
  }

  const std::string_view rest = uri.substr(std::string_view("file://").size());
  // For file:///, keep the leading slash. For file://hostname/ we don't support hostnames.
  if (starts_with(rest, "/")) {
    // ok
  } else {
    // file://host/path is not supported
    return std::nullopt;
  }

  const std::string decoded = percent_decode(rest);
  return fs::path(decoded);
}

std::optional<std::string> read_file_text(const fs::path & p)
{
  std::ifstream ifs(p, std::ios::in | std::ios::binary);  // NOLINT(misc-const-correctness)
  if (!ifs) return std::nullopt;
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

// -----------------------------
// UTF-8 <-> LSP (UTF-16) position conversion
// -----------------------------

struct LspPos
{
  int line = 0;
  int character = 0;  // UTF-16 code units
};

struct LspRange
{
  LspPos start;
  LspPos end;
};

// Decode one UTF-8 code point starting at i.
// Returns {codepoint, bytes_consumed}. On invalid input, consumes 1 byte.
std::pair<uint32_t, size_t> decode_utf8(std::string_view s, size_t i)
{
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80) {
    return {b0, 1};
  }
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if ((b1 & 0xC0) == 0x80) {
      const uint32_t cp =
        (static_cast<uint32_t>(b0 & 0x1F) << 6) | static_cast<uint32_t>(b1 & 0x3F);
      return {cp, 2};
    }
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    const auto b2 = static_cast<unsigned char>(s[i + 2]);
    if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80)) {
      const uint32_t cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
                          (static_cast<uint32_t>(b1 & 0x3F) << 6) |
                          static_cast<uint32_t>(b2 & 0x3F);
      return {cp, 3};
    }
  }
  if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    const auto b2 = static_cast<unsigned char>(s[i + 2]);
    const auto b3 = static_cast<unsigned char>(s[i + 3]);
    if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80) && ((b3 & 0xC0) == 0x80)) {
      const uint32_t cp =
        (static_cast<uint32_t>(b0 & 0x07) << 18) | (static_cast<uint32_t>(b1 & 0x3F) << 12) |
        (static_cast<uint32_t>(b2 & 0x3F) << 6) | static_cast<uint32_t>(b3 & 0x3F);
      return {cp, 4};
    }
  }
  return {0xFFFDU, 1};
}

int utf16_units(uint32_t cp) { return (cp > 0xFFFFU) ? 2 : 1; }

LspPos lsp_pos_at_utf8_byte(std::string_view text, uint32_t target_byte)
{
  LspPos pos;

  uint32_t bytes = 0;
  size_t i = 0;
  while (i < text.size() && bytes < target_byte) {
    const auto [cp, consumed] = decode_utf8(text, i);
    const auto utf8_units = static_cast<uint32_t>(consumed);

    if (bytes + utf8_units > target_byte) {
      break;
    }

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

uint32_t utf8_byte_at_lsp_pos(std::string_view text, const LspPos & target)
{
  if (target.line < 0 || target.character < 0) {
    return 0;
  }

  int line = 0;
  int ch16 = 0;

  uint32_t bytes = 0;
  size_t i = 0;
  while (i < text.size()) {
    if (line > target.line) {
      break;
    }
    if (line == target.line && ch16 >= target.character) {
      break;
    }

    const auto [cp, consumed] = decode_utf8(text, i);

    // If we are on the target line, ensure we don't cross the character boundary.
    if (line == target.line) {
      const int next = ch16 + utf16_units(cp);
      if (next > target.character) {
        break;
      }
      ch16 = next;
    }

    bytes += static_cast<uint32_t>(consumed);
    i += consumed;

    if (cp == '\n') {
      line += 1;
      ch16 = 0;
    }
  }

  if (bytes > text.size()) {
    return static_cast<uint32_t>(text.size());
  }
  return bytes;
}

LspRange lsp_range_from_byte_range(std::string_view text, uint32_t start_byte, uint32_t end_byte)
{
  LspRange r;
  r.start = lsp_pos_at_utf8_byte(text, start_byte);
  r.end = lsp_pos_at_utf8_byte(text, end_byte);
  return r;
}

// -----------------------------
// JSON-RPC connection (Content-Length framing)
// -----------------------------

struct RpcMessage
{
  json payload;
};

class JsonRpcConnection
{
public:
  explicit JsonRpcConnection(std::istream & in, std::ostream & out) : in_(in), out_(out) {}

  std::optional<RpcMessage> read_message()
  {
    std::string line;
    int content_length = -1;

    // Read headers
    while (std::getline(in_, line)) {
      line = trim_cr(line);
      if (line.empty()) {
        break;
      }

      const std::string_view sv(line);
      constexpr std::string_view k_content_length = "Content-Length:";
      if (starts_with(sv, k_content_length)) {
        std::string_view rest = sv.substr(k_content_length.size());
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
          rest.remove_prefix(1);
        }
        content_length = std::atoi(std::string(rest).c_str());
      }
    }

    if (!in_) {
      return std::nullopt;
    }

    if (content_length <= 0) {
      // Invalid message. Try to continue.
      return std::nullopt;
    }

    std::string body;
    body.resize(static_cast<size_t>(content_length));
    in_.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (in_.gcount() != static_cast<std::streamsize>(body.size())) {
      return std::nullopt;
    }

    RpcMessage msg;
    try {
      msg.payload = json::parse(body);
    } catch (...) {
      return std::nullopt;
    }
    return msg;
  }

  void write_response(const json & id, const json & result)
  {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    write_payload(resp);
  }

  void write_error(const json & id, int code, std::string message)
  {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = json{{"code", code}, {"message", std::move(message)}};
    write_payload(resp);
  }

  void write_notification(std::string method, json params)
  {
    json note;
    note["jsonrpc"] = "2.0";
    note["method"] = std::move(method);
    note["params"] = std::move(params);
    write_payload(note);
  }

private:
  void write_payload(const json & payload)
  {
    const std::string body = payload.dump();
    out_ << "Content-Length: " << body.size() << "\r\n\r\n";
    out_ << body;
    out_.flush();
  }

  std::istream & in_;
  std::ostream & out_;
};

// -----------------------------
// LSP server glue
// -----------------------------

int completion_kind_to_lsp(std::string_view k)
{
  // LSP CompletionItemKind
  // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#completionItemKind
  if (k == "Port") return 5;      // Field
  if (k == "Node") return 3;      // Function
  if (k == "Variable") return 6;  // Variable
  if (k == "Keyword") return 14;  // Keyword
  return 1;                       // Text
}

int symbol_kind_to_lsp(std::string_view k)
{
  // LSP SymbolKind
  if (k == "Tree") return 12;       // Function
  if (k == "Declare") return 5;     // Class
  if (k == "GlobalVar") return 13;  // Variable
  return 19;                        // Object
}

int highlight_kind_to_lsp(std::string_view k)
{
  // LSP DocumentHighlightKind: Text=1, Read=2, Write=3
  if (k == "Write") return 3;
  if (k == "Text") return 1;
  return 2;
}

int diag_severity_to_lsp(std::string_view k)
{
  // LSP DiagnosticSeverity: Error=1, Warning=2, Information=3, Hint=4
  if (k == "Error") return 1;
  if (k == "Warning") return 2;
  if (k == "Info") return 3;
  if (k == "Hint") return 4;
  return 1;
}

struct ServerState
{
  bt_dsl::lsp::Workspace ws;
  std::unordered_map<std::string, std::string> docs;  // uri -> utf8 text
  std::string stdlib_uri;
  std::optional<fs::path> stdlib_path;
  bool shutdown_requested = false;

  static std::vector<std::string> parse_uris(const std::string & raw)
  {
    try {
      const json j = json::parse(raw);
      if (!j.is_object()) return {};
      const auto it = j.find("uris");
      if (it == j.end() || !it->is_array()) return {};
      std::vector<std::string> out;
      for (const auto & v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
      }
      return out;
    } catch (...) {
      return {};
    }
  }

  void ensure_doc_loaded_from_disk(const std::string & uri)
  {
    if (docs.find(uri) != docs.end()) {
      return;
    }

    const auto p = file_uri_to_path(uri);
    if (!p) {
      return;
    }

    const auto txt = read_file_text(*p);
    if (!txt) {
      return;
    }

    docs[uri] = *txt;
    ws.set_document(uri, *txt);
  }

  void ensure_stdlib_loaded()
  {
    if (stdlib_uri.empty()) {
      return;
    }

    if (docs.find(stdlib_uri) != docs.end()) {
      return;
    }

    if (stdlib_path) {
      const auto txt = read_file_text(*stdlib_path);
      if (txt) {
        docs[stdlib_uri] = *txt;
        ws.set_document(stdlib_uri, *txt);
      }
      return;
    }

    // As a fallback, try to read from file:// URI if possible.
    ensure_doc_loaded_from_disk(stdlib_uri);
  }

  std::vector<std::string> ensure_imports_loaded(const std::string & uri)
  {
    ensure_stdlib_loaded();

    // Reference spec: import visibility is non-transitive (docs/reference/declarations-and-scopes.md 4.1.3).
    // Therefore, we return only *direct* imports of `uri` (plus optional stdlib).
    // However, for analysis/navigation convenience, we still try to ensure the full
    // transitive import closure is loaded into the workspace.

    const std::vector<std::string> imported_direct =
      parse_uris(ws.resolve_imports_json(uri, stdlib_uri));

    std::unordered_set<std::string> visited;
    visited.insert(uri);
    if (!stdlib_uri.empty()) visited.insert(stdlib_uri);

    std::vector<std::string> queue;
    queue.reserve(imported_direct.size());

    for (const auto & u : imported_direct) {
      if (visited.insert(u).second) {
        queue.push_back(u);
      }
    }

    // BFS over direct imports of each document.
    for (size_t qi = 0; qi < queue.size() && qi < 256; ++qi) {
      const std::string cur = queue[qi];

      // Make sure the document exists in the workspace before asking it for its imports.
      ensure_doc_loaded_from_disk(cur);

      const auto next = parse_uris(ws.resolve_imports_json(cur, stdlib_uri));
      for (const auto & u : next) {
        if (visited.insert(u).second) {
          queue.push_back(u);
        }
      }
    }

    return imported_direct;
  }

  std::optional<std::string_view> get_doc_text(std::string_view uri) const
  {
    const auto it = docs.find(std::string(uri));
    if (it == docs.end()) return std::nullopt;
    return std::string_view(it->second);
  }
};

json lsp_position_json(const LspPos & p)
{
  return json{{"line", p.line}, {"character", p.character}};
}

json lsp_range_json(const LspRange & r)
{
  return json{{"start", lsp_position_json(r.start)}, {"end", lsp_position_json(r.end)}};
}

json convert_diagnostics(std::string_view diag_raw, std::string_view doc_text)
{
  json diag_items = json::array();
  try {
    const json j = json::parse(diag_raw);
    const auto it = j.find("items");
    if (it == j.end() || !it->is_array()) {
      return diag_items;
    }

    for (const auto & item : *it) {
      if (!item.is_object()) continue;
      const auto msg_it = item.find("message");
      const auto sev_it = item.find("severity");
      const auto range_it = item.find("range");
      if (msg_it == item.end() || !msg_it->is_string()) continue;
      if (sev_it == item.end() || !sev_it->is_string()) continue;
      if (range_it == item.end() || !range_it->is_object()) continue;

      const uint32_t sb = range_it->value("startByte", uint32_t{0});
      const uint32_t eb = range_it->value("endByte", uint32_t{0});
      const auto r = lsp_range_from_byte_range(doc_text, sb, eb);

      json d;
      d["range"] = lsp_range_json(r);
      d["message"] = msg_it->get<std::string>();
      d["severity"] = diag_severity_to_lsp(sev_it->get<std::string>());
      if (item.contains("source") && item["source"].is_string()) {
        d["source"] = item["source"].get<std::string>();
      } else {
        d["source"] = "bt-dsl";
      }
      if (item.contains("code") && item["code"].is_string()) {
        d["code"] = item["code"].get<std::string>();
      }

      diag_items.push_back(std::move(d));
    }
  } catch (...) {
    // Best-effort: malformed diagnostics should not crash the server.
    (void)0;
  }

  return diag_items;
}

void publish_diagnostics(
  JsonRpcConnection & conn, ServerState & st, const std::string & uri,
  std::string_view fallback_text)
{
  const auto imported = st.ensure_imports_loaded(uri);
  const std::string diag_raw = st.ws.diagnostics_json(uri, imported);

  const auto doc_text_opt = st.get_doc_text(uri);
  const std::string_view doc_text = doc_text_opt ? *doc_text_opt : fallback_text;

  json diag_note;
  diag_note["uri"] = uri;
  diag_note["diagnostics"] = convert_diagnostics(diag_raw, doc_text);
  conn.write_notification("textDocument/publishDiagnostics", std::move(diag_note));
}

void handle_initialize(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  // initializationOptions: { stdlibPath?: string, stdlibUri?: string }
  try {
    if (params.contains("initializationOptions") && params["initializationOptions"].is_object()) {
      const auto & io = params["initializationOptions"];
      if (io.contains("stdlibPath") && io["stdlibPath"].is_string()) {
        st.stdlib_path = fs::path(io["stdlibPath"].get<std::string>());
        // Build file:// URI from path (best-effort, no percent-encoding).
        const fs::path abs = fs::absolute(*st.stdlib_path);
        st.stdlib_uri = std::string("file://") + abs.string();
      } else if (io.contains("stdlibUri") && io["stdlibUri"].is_string()) {
        st.stdlib_uri = io["stdlibUri"].get<std::string>();
      }
    }
  } catch (const std::exception & e) {
    log_handler_error("initialize", &e);
  } catch (...) {
    log_handler_error("initialize", nullptr);
  }

  json caps;
  caps["textDocumentSync"] = json{{"openClose", true}, {"change", 1}};  // Full
  caps["completionProvider"] = json{{"resolveProvider", false}};
  caps["hoverProvider"] = true;
  caps["definitionProvider"] = true;
  caps["documentSymbolProvider"] = true;
  caps["documentHighlightProvider"] = true;
  caps["semanticTokensProvider"] = json{
    {"legend", json{{"tokenTypes", k_semantic_types}, {"tokenModifiers", k_semantic_mods}}},
    {"full", true},
  };

  json result;
  result["capabilities"] = caps;
  result["serverInfo"] = json{{"name", "bt-dsl-lsp"}, {"version", "0.1.0"}};

  conn.write_response(id, result);
}

void handle_did_open(const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const auto & td = params.at("textDocument");
  const std::string uri = td.at("uri").get<std::string>();
  const std::string text = td.at("text").get<std::string>();

  st.docs[uri] = text;
  st.ws.set_document(uri, text);
  publish_diagnostics(conn, st, uri, text);
}

void handle_did_change(const json & params, JsonRpcConnection & conn, ServerState & st)
{
  // We advertise full sync; take the whole text from contentChanges[0].text.
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const auto & changes = params.at("contentChanges");
  if (!changes.is_array() || changes.empty()) {
    return;
  }
  const std::string text = changes[0].at("text").get<std::string>();

  st.docs[uri] = text;
  st.ws.set_document(uri, text);
  publish_diagnostics(conn, st, uri, st.docs[uri]);
}

void handle_did_close(const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  st.docs.erase(uri);
  st.ws.remove_document(uri);

  json diag_note;
  diag_note["uri"] = uri;
  diag_note["diagnostics"] = json::array();
  conn.write_notification("textDocument/publishDiagnostics", std::move(diag_note));
}

void handle_completion(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const int line = params.at("position").at("line").get<int>();
  const int character = params.at("position").at("character").get<int>();

  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, json{{"isIncomplete", false}, {"items", json::array()}});
    return;
  }

  const std::string_view text = doc_it->second;
  const uint32_t byte_off = utf8_byte_at_lsp_pos(text, LspPos{line, character});
  const auto imported = st.ensure_imports_loaded(uri);

  const std::string raw = st.ws.completion_json(uri, byte_off, imported);
  json out;
  out["isIncomplete"] = false;
  out["items"] = json::array();

  try {
    const json j = json::parse(raw);
    if (j.is_object()) {
      if (j.contains("isIncomplete")) {
        out["isIncomplete"] = j["isIncomplete"].get<bool>();
      }
      const auto it = j.find("items");
      if (it != j.end() && it->is_array()) {
        json items = json::array();
        for (const auto & item : *it) {
          if (!item.is_object()) continue;
          if (!item.contains("label") || !item["label"].is_string()) continue;

          const std::string label = item["label"].get<std::string>();
          const std::string insert_text = item.value("insertText", label);

          json ci;
          ci["label"] = label;
          if (item.contains("detail") && item["detail"].is_string()) {
            ci["detail"] = item["detail"].get<std::string>();
          }
          if (item.contains("kind") && item["kind"].is_string()) {
            ci["kind"] = completion_kind_to_lsp(item["kind"].get<std::string>());
          }

          // If core suggests a replace range, convert to a textEdit.
          if (item.contains("replaceRange") && item["replaceRange"].is_object()) {
            const auto & rr = item["replaceRange"];
            const uint32_t sb = rr.value("startByte", byte_off);
            const uint32_t eb = rr.value("endByte", byte_off);
            const auto range = lsp_range_from_byte_range(text, sb, eb);
            ci["textEdit"] = json{{"range", lsp_range_json(range)}, {"newText", insert_text}};
          } else {
            ci["insertText"] = insert_text;
          }

          items.push_back(std::move(ci));
        }
        out["items"] = std::move(items);
      }
    }
  } catch (...) {
    // best-effort
    (void)0;
  }

  conn.write_response(id, out);
}

void handle_hover(const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const int line = params.at("position").at("line").get<int>();
  const int character = params.at("position").at("character").get<int>();

  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, nullptr);
    return;
  }

  const std::string_view text = doc_it->second;
  const uint32_t byte_off = utf8_byte_at_lsp_pos(text, LspPos{line, character});
  const auto imported = st.ensure_imports_loaded(uri);

  const std::string raw = st.ws.hover_json(uri, byte_off, imported);
  try {
    const json j = json::parse(raw);
    if (!j.is_object()) {
      conn.write_response(id, nullptr);
      return;
    }

    if (!j.contains("contents") || j["contents"].is_null()) {
      conn.write_response(id, nullptr);
      return;
    }

    json hover;
    hover["contents"] = json{{"kind", "markdown"}, {"value", j["contents"].get<std::string>()}};

    if (j.contains("range") && j["range"].is_object()) {
      const auto & rr = j["range"];
      const uint32_t sb = rr.value("startByte", byte_off);
      const uint32_t eb = rr.value("endByte", byte_off);
      hover["range"] = lsp_range_json(lsp_range_from_byte_range(text, sb, eb));
    }

    conn.write_response(id, hover);
  } catch (...) {
    conn.write_response(id, nullptr);
  }
}

void handle_definition(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const int line = params.at("position").at("line").get<int>();
  const int character = params.at("position").at("character").get<int>();

  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, json::array());
    return;
  }

  const std::string_view text = doc_it->second;
  const uint32_t byte_off = utf8_byte_at_lsp_pos(text, LspPos{line, character});
  const auto imported = st.ensure_imports_loaded(uri);

  const std::string raw = st.ws.definition_json(uri, byte_off, imported);
  json locs = json::array();
  try {
    const json j = json::parse(raw);
    const auto it = j.find("locations");
    if (it != j.end() && it->is_array()) {
      for (const auto & loc : *it) {
        if (!loc.is_object()) continue;
        if (!loc.contains("uri") || !loc["uri"].is_string()) continue;
        if (!loc.contains("range") || !loc["range"].is_object()) continue;

        const std::string turi = loc["uri"].get<std::string>();
        const uint32_t sb = loc["range"].value("startByte", uint32_t{0});
        const uint32_t eb = loc["range"].value("endByte", uint32_t{0});

        // Ensure target doc is loaded so we can convert ranges.
        st.ensure_doc_loaded_from_disk(turi);
        const auto ttext_opt = st.get_doc_text(turi);
        const std::string_view ttext = ttext_opt ? *ttext_opt : std::string_view();

        json out_loc;
        out_loc["uri"] = turi;
        out_loc["range"] = lsp_range_json(lsp_range_from_byte_range(ttext, sb, eb));
        locs.push_back(std::move(out_loc));
      }
    }
  } catch (...) {
    // best-effort
    (void)0;
  }

  conn.write_response(id, locs);
}

void handle_document_symbol(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, json::array());
    return;
  }

  const std::string_view text = doc_it->second;
  const std::string raw = st.ws.document_symbols_json(uri);

  json out = json::array();
  try {
    const json j = json::parse(raw);
    const auto it = j.find("symbols");
    if (it != j.end() && it->is_array()) {
      for (const auto & sym : *it) {
        if (!sym.is_object()) continue;
        if (!sym.contains("name") || !sym["name"].is_string()) continue;
        if (!sym.contains("kind") || !sym["kind"].is_string()) continue;
        if (!sym.contains("range") || !sym["range"].is_object()) continue;

        const uint32_t sb = sym["range"].value("startByte", uint32_t{0});
        const uint32_t eb = sym["range"].value("endByte", uint32_t{0});
        const auto r = lsp_range_from_byte_range(text, sb, eb);

        LspRange sel = r;
        if (sym.contains("selectionRange") && sym["selectionRange"].is_object()) {
          const auto & sr = sym["selectionRange"];
          const uint32_t ssb = sr.value("startByte", sb);
          const uint32_t seb = sr.value("endByte", eb);
          sel = lsp_range_from_byte_range(text, ssb, seb);
        }

        json ds;
        ds["name"] = sym["name"].get<std::string>();
        ds["kind"] = symbol_kind_to_lsp(sym["kind"].get<std::string>());
        ds["range"] = lsp_range_json(r);
        ds["selectionRange"] = lsp_range_json(sel);
        ds["children"] = json::array();

        out.push_back(std::move(ds));
      }
    }
  } catch (...) {
    // best-effort
    (void)0;
  }

  conn.write_response(id, out);
}

void handle_document_highlight(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const int line = params.at("position").at("line").get<int>();
  const int character = params.at("position").at("character").get<int>();

  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, json::array());
    return;
  }

  const std::string_view text = doc_it->second;
  const uint32_t byte_off = utf8_byte_at_lsp_pos(text, LspPos{line, character});
  const auto imported = st.ensure_imports_loaded(uri);

  const std::string raw = st.ws.document_highlights_json(uri, byte_off, imported);

  json out = json::array();
  try {
    const json j = json::parse(raw);
    const auto it = j.find("items");
    if (it != j.end() && it->is_array()) {
      for (const auto & item : *it) {
        if (!item.is_object()) continue;
        if (!item.contains("range") || !item["range"].is_object()) continue;
        const uint32_t sb = item["range"].value("startByte", byte_off);
        const uint32_t eb = item["range"].value("endByte", byte_off);

        json dh;
        dh["range"] = lsp_range_json(lsp_range_from_byte_range(text, sb, eb));
        if (item.contains("kind") && item["kind"].is_string()) {
          dh["kind"] = highlight_kind_to_lsp(item["kind"].get<std::string>());
        }
        out.push_back(std::move(dh));
      }
    }
  } catch (...) {
    // best-effort
    (void)0;
  }

  conn.write_response(id, out);
}

void handle_semantic_tokens_full(
  const json & id, const json & params, JsonRpcConnection & conn, ServerState & st)
{
  const std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const auto doc_it = st.docs.find(uri);
  if (doc_it == st.docs.end()) {
    conn.write_response(id, json{{"data", json::array()}});
    return;
  }

  const std::string_view text = doc_it->second;
  const auto imported = st.ensure_imports_loaded(uri);
  const std::string raw = st.ws.semantic_tokens_json(uri, imported);

  struct Tok
  {
    LspPos start;
    LspPos end;
    int type_idx = -1;
    int mod_bits = 0;
  };

  std::vector<Tok> toks;
  try {
    const json j = json::parse(raw);
    const auto it = j.find("tokens");
    if (it != j.end() && it->is_array()) {
      for (const auto & t : *it) {
        if (!t.is_object()) continue;
        if (!t.contains("range") || !t["range"].is_object()) continue;
        if (!t.contains("type") || !t["type"].is_string()) continue;

        const uint32_t sb = t["range"].value("startByte", uint32_t{0});
        const uint32_t eb = t["range"].value("endByte", uint32_t{0});
        const auto range = lsp_range_from_byte_range(text, sb, eb);

        // Only single-line tokens are supported.
        if (range.start.line != range.end.line) continue;

        const std::string type = t["type"].get<std::string>();
        const auto ti = std::find(k_semantic_types.begin(), k_semantic_types.end(), type);
        if (ti == k_semantic_types.end()) continue;

        Tok tok;
        tok.start = range.start;
        tok.end = range.end;
        tok.type_idx = static_cast<int>(std::distance(k_semantic_types.begin(), ti));
        tok.mod_bits = 0;

        if (t.contains("modifiers") && t["modifiers"].is_array()) {
          for (const auto & mv : t["modifiers"]) {
            if (!mv.is_string()) continue;
            const std::string mod = mv.get<std::string>();
            const auto mi = std::find(k_semantic_mods.begin(), k_semantic_mods.end(), mod);
            if (mi != k_semantic_mods.end()) {
              const int bit = static_cast<int>(std::distance(k_semantic_mods.begin(), mi));
              tok.mod_bits |= 1 << bit;
            }
          }
        }

        toks.push_back(tok);
      }
    }
  } catch (...) {
    // best-effort
    (void)0;
  }

  std::sort(toks.begin(), toks.end(), [](const Tok & a, const Tok & b) {
    if (a.start.line != b.start.line) return a.start.line < b.start.line;
    return a.start.character < b.start.character;
  });

  // LSP semantic tokens are encoded as a flat int array:
  // [deltaLine, deltaStartChar, length, tokenType, tokenModifiers]
  json data = json::array();
  int prev_line = 0;
  int prev_char = 0;

  bool first = true;
  for (const auto & t : toks) {
    const int line = t.start.line;
    const int start_char = t.start.character;
    const int len = t.end.character - t.start.character;
    if (len <= 0) continue;

    const int delta_line = first ? line : (line - prev_line);
    const int delta_start = (first || delta_line != 0) ? start_char : (start_char - prev_char);

    data.push_back(delta_line);
    data.push_back(delta_start);
    data.push_back(len);
    data.push_back(t.type_idx);
    data.push_back(t.mod_bits);

    prev_line = line;
    prev_char = start_char;
    first = false;
  }

  conn.write_response(id, json{{"data", std::move(data)}});
}

std::optional<int> handle_message(
  JsonRpcConnection & conn, ServerState & st, const std::string & method, bool is_request,
  const json & id, const json & params)
{
  auto reply_invalid_params = [&]() {
    if (is_request) conn.write_error(id, -32602, "Invalid params");
  };

  // -----------------------------
  // Lifecycle
  // -----------------------------

  if (method == "initialize") {
    try {
      handle_initialize(id, params, conn, st);
    } catch (const std::exception & e) {
      log_handler_error("initialize", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("initialize", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "initialized") {
    return std::nullopt;
  }

  if (method == "shutdown") {
    st.shutdown_requested = true;
    if (is_request) {
      conn.write_response(id, nullptr);
    }
    return std::nullopt;
  }

  if (method == "exit") {
    return st.shutdown_requested ? 0 : 1;
  }

  // -----------------------------
  // Text document sync
  // -----------------------------

  if (method == "textDocument/didOpen") {
    try {
      handle_did_open(params, conn, st);
    } catch (const std::exception & e) {
      log_handler_error("textDocument/didOpen", &e);
    } catch (...) {
      log_handler_error("textDocument/didOpen", nullptr);
    }
    return std::nullopt;
  }

  if (method == "textDocument/didChange") {
    try {
      handle_did_change(params, conn, st);
    } catch (const std::exception & e) {
      log_handler_error("textDocument/didChange", &e);
    } catch (...) {
      log_handler_error("textDocument/didChange", nullptr);
    }
    return std::nullopt;
  }

  if (method == "textDocument/didClose") {
    try {
      handle_did_close(params, conn, st);
    } catch (const std::exception & e) {
      log_handler_error("textDocument/didClose", &e);
    } catch (...) {
      log_handler_error("textDocument/didClose", nullptr);
    }
    return std::nullopt;
  }

  // -----------------------------
  // Language features
  // -----------------------------

  if (method == "textDocument/completion") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_completion(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/completion", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/completion", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/completion", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "textDocument/hover") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_hover(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/hover", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/hover", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/hover", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "textDocument/definition") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_definition(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/definition", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/definition", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/definition", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "textDocument/documentSymbol") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_document_symbol(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/documentSymbol", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/documentSymbol", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/documentSymbol", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "textDocument/documentHighlight") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_document_highlight(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/documentHighlight", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/documentHighlight", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/documentHighlight", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  if (method == "textDocument/semanticTokens/full") {
    if (!is_request) {
      return std::nullopt;
    }
    try {
      handle_semantic_tokens_full(id, params, conn, st);
    } catch (const json::exception & e) {
      log_handler_error("textDocument/semanticTokens/full", &e);
      reply_invalid_params();
    } catch (const std::exception & e) {
      log_handler_error("textDocument/semanticTokens/full", &e);
      conn.write_error(id, -32603, "Internal error");
    } catch (...) {
      log_handler_error("textDocument/semanticTokens/full", nullptr);
      conn.write_error(id, -32603, "Internal error");
    }
    return std::nullopt;
  }

  // Unknown request: return method not found
  if (is_request) {
    conn.write_error(id, -32601, "Method not found");
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char ** argv)
try {
  (void)argc;
  (void)argv;

  // LSP servers must not buffer stdio too aggressively.
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  JsonRpcConnection conn(std::cin, std::cout);
  ServerState st;

  // A very small, robust-ish message loop.
  for (;;) {
    const auto msg_opt = conn.read_message();
    if (!msg_opt) {
      if (!std::cin.good()) {
        break;
      }
      continue;
    }

    const json & m = msg_opt->payload;
    const auto method_it = m.find("method");
    const bool has_method = method_it != m.end() && method_it->is_string();
    const std::string method = has_method ? method_it->get<std::string>() : std::string();

    const bool is_request = m.contains("id");
    const json id = is_request ? m.at("id") : json();
    const json params = m.contains("params") ? m.at("params") : json::object();

    const auto exit_code = handle_message(conn, st, method, is_request, id, params);
    if (exit_code) {
      return *exit_code;
    }
  }

  return 0;
}

catch (const std::exception & e) {
  std::cerr << "bt_dsl_lsp_server: fatal: " << e.what() << "\n";
  return 1;
} catch (...) {
  std::cerr << "bt_dsl_lsp_server: fatal: unknown error\n";
  return 1;
}
