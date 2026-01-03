// BT-DSL LSP server (stdio JSON-RPC)
//
// This is a thin wrapper around bt_dsl::lsp::Workspace (serverless APIs).
// It implements the subset of LSP needed by the VS Code extension e2e tests.
//
#include <bt_dsl/basic/source_manager.hpp>
#include <bt_dsl/driver/stdlib_finder.hpp>
#include <bt_dsl/lsp/lsp.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using nlohmann::json;

namespace
{

struct DocState
{
  std::string uri;
  std::string text;
  std::vector<uint32_t> line_offsets;      // byte offsets of each line start
  std::vector<std::string> imported_uris;  // resolved direct imports (+ stdlib)
};

std::vector<uint32_t> build_line_offsets(std::string_view text)
{
  std::vector<uint32_t> offsets;
  offsets.push_back(0);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      offsets.push_back(static_cast<uint32_t>(i + 1));
    }
  }
  return offsets;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

int hex_to_int(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::string url_decode(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.size()) {
      const int hi = hex_to_int(s[i + 1]);
      const int lo = hex_to_int(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (c == '+') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::optional<std::string> file_uri_to_path(std::string_view uri)
{
  // Minimal file URI decoding for Linux/macOS paths.
  // Examples:
  //   file:///home/user/a.bt
  //   file:/home/user/a.bt (rare)
  if (!starts_with(uri, "file:")) {
    return std::nullopt;
  }

  std::string_view rest = uri.substr(std::string_view("file:").size());
  if (starts_with(rest, "///")) {
    rest = rest.substr(2);  // keep one leading slash
  } else if (starts_with(rest, "//")) {
    // file://hostname/path is not supported here.
    return std::nullopt;
  }

  // Now rest should start with '/'
  return url_decode(rest);
}

std::string path_to_file_uri(const std::string & path)
{
  // Good enough for local absolute paths on Linux.
  // NOTE: This does not percent-encode.
  if (!path.empty() && path[0] == '/') {
    return std::string("file://") + path;
  }
  return std::string("file:///") + path;
}

namespace fs = std::filesystem;

// Resolve bt-dsl-pkg://pkg/path.bt to file:// URI using the stdlib base directory.
// The stdlib_base should be the parent of `std/` (e.g., /path/to/core for /path/to/core/std/)
std::optional<std::string> resolve_package_uri(
  std::string_view uri, const std::string & stdlib_base)
{
  constexpr std::string_view pkg_prefix = "bt-dsl-pkg://";
  if (!starts_with(uri, pkg_prefix)) {
    return std::nullopt;
  }
  if (stdlib_base.empty()) {
    return std::nullopt;
  }

  // bt-dsl-pkg://std/nodes.bt -> <stdlib_base>/std/nodes.bt
  const std::string_view pkg_path = uri.substr(pkg_prefix.size());
  std::error_code ec;
  const fs::path resolved = fs::path(stdlib_base) / pkg_path;
  if (fs::exists(resolved, ec) && !ec) {
    return path_to_file_uri(fs::canonical(resolved, ec).string());
  }
  return std::nullopt;
}

std::optional<std::string> read_file_to_string(const std::string & path)
{
  std::ifstream f(path, std::ios::in | std::ios::binary);
  if (!f.is_open()) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int lsp_severity(std::string_view s)
{
  // LSP DiagnosticSeverity:
  // 1 Error, 2 Warning, 3 Information, 4 Hint
  if (s == "Error") return 1;
  if (s == "Warning") return 2;
  if (s == "Info") return 3;
  if (s == "Hint") return 4;
  return 3;
}

int completion_kind(std::string_view s)
{
  // LSP CompletionItemKind (subset)
  if (s == "Keyword") return 14;
  if (s == "Variable") return 6;
  if (s == "Function") return 3;
  if (s == "Method") return 2;
  if (s == "Field") return 5;
  if (s == "Property") return 10;
  if (s == "Class") return 7;
  if (s == "Interface") return 8;
  if (s == "Module") return 9;
  if (s == "Enum") return 13;
  return 1;  // Text
}

int symbol_kind(std::string_view s)
{
  // LSP SymbolKind (subset)
  if (s == "Tree") return 12;         // Function
  if (s == "Declare") return 13;      // Variable (extern node/type-ish)
  if (s == "GlobalVar") return 13;    // Variable
  if (s == "GlobalConst") return 14;  // Constant
  return 13;
}

json to_lsp_range_from_full_range(const json & fr)
{
  // FullSourceRange uses 1-indexed line/column; LSP uses 0-indexed.
  const int sl = std::max(0, fr.value("startLine", 1) - 1);
  const int sc = std::max(0, fr.value("startColumn", 1) - 1);
  const int el = std::max(0, fr.value("endLine", 1) - 1);
  const int ec = std::max(0, fr.value("endColumn", 1) - 1);

  return json{
    {"start", json{{"line", sl}, {"character", sc}}},
    {"end", json{{"line", el}, {"character", ec}}},
  };
}

std::optional<uint32_t> utf8_position_to_byte_offset(
  const DocState & doc, uint32_t line, uint32_t character)
{
  if (doc.line_offsets.empty()) {
    return std::nullopt;
  }
  if (line >= doc.line_offsets.size()) {
    return static_cast<uint32_t>(doc.text.size());
  }

  const uint32_t line_start = doc.line_offsets[line];
  const uint32_t next_line_start = (line + 1 < doc.line_offsets.size())
                                     ? doc.line_offsets[line + 1]
                                     : static_cast<uint32_t>(doc.text.size());

  const uint32_t line_len = (next_line_start >= line_start) ? (next_line_start - line_start) : 0;
  const uint32_t col = std::min<uint32_t>(character, line_len);
  return line_start + col;
}

std::optional<uint32_t> utf16_position_to_byte_offset(
  const DocState & doc, uint32_t line, uint32_t character)
{
  if (doc.line_offsets.empty()) {
    return std::nullopt;
  }
  if (line >= doc.line_offsets.size()) {
    return static_cast<uint32_t>(doc.text.size());
  }

  const uint32_t line_start = doc.line_offsets[line];
  const uint32_t next_line_start = (line + 1 < doc.line_offsets.size())
                                     ? doc.line_offsets[line + 1]
                                     : static_cast<uint32_t>(doc.text.size());

  const std::string_view slice =
    std::string_view(doc.text).substr(line_start, next_line_start - line_start);

  uint32_t utf16_units = 0;
  uint32_t byte_index = 0;

  auto advance = [&](uint32_t bytes, uint32_t units) {
    if (utf16_units + units > character) {
      // Target is inside this code point; clamp to current byte index.
      return false;
    }
    utf16_units += units;
    byte_index += bytes;
    return true;
  };

  while (byte_index < slice.size() && utf16_units < character) {
    const auto c0 = static_cast<unsigned char>(slice[byte_index]);

    // Decode a single UTF-8 code point.
    uint32_t cp = c0;
    uint32_t nbytes = 1;
    if (c0 >= 0x80 && (c0 & 0xE0) == 0xC0 && byte_index + 1 < slice.size()) {
      cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(slice[byte_index + 1]) & 0x3F);
      nbytes = 2;
    } else if (c0 >= 0x80 && (c0 & 0xF0) == 0xE0 && byte_index + 2 < slice.size()) {
      cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(slice[byte_index + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(slice[byte_index + 2]) & 0x3F);
      nbytes = 3;
    } else if (c0 >= 0x80 && (c0 & 0xF8) == 0xF0 && byte_index + 3 < slice.size()) {
      cp = ((c0 & 0x07) << 18) |
           ((static_cast<unsigned char>(slice[byte_index + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(slice[byte_index + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(slice[byte_index + 3]) & 0x3F);
      nbytes = 4;
    }

    const uint32_t units = (cp <= 0xFFFF) ? 1U : 2U;
    if (!advance(nbytes, units)) {
      break;
    }
  }

  return line_start + std::min<uint32_t>(byte_index, static_cast<uint32_t>(slice.size()));
}

void write_message(const json & msg)
{
  const std::string body = msg.dump();
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
  std::cout << body;
  std::cout.flush();
}

std::optional<json> read_message()
{
  std::string line;
  size_t content_length = 0;
  bool saw_length = false;

  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.empty()) {
      break;
    }

    const std::string_view sv(line);
    if (starts_with(sv, "Content-Length:")) {
      const std::string_view rest = sv.substr(std::string_view("Content-Length:").size());
      content_length = static_cast<size_t>(std::strtoul(std::string(rest).c_str(), nullptr, 10));
      saw_length = true;
    }
  }

  if (!saw_length || content_length == 0) {
    if (!std::cin.good()) {
      return std::nullopt;
    }
    // Ignore malformed message.
    return std::nullopt;
  }

  std::string body(content_length, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(content_length));
  if (std::cin.gcount() != static_cast<std::streamsize>(content_length)) {
    return std::nullopt;
  }

  try {
    return json::parse(body);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

int main()
{
  try {
    bt_dsl::lsp::Workspace ws;

    std::unordered_map<std::string, DocState> docs;
    std::string stdlib_base;  // Base directory containing std/ (parent of stdlib dir)
    std::string negotiated_position_encoding = "utf-8";

    auto upsert_doc = [&](const std::string & uri, const std::string & text) -> DocState & {
      auto & d = docs[uri];
      d.uri = uri;
      d.text = text;
      d.line_offsets = build_line_offsets(d.text);
      ws.set_document(uri, text);
      return d;
    };

    auto ensure_loaded = [&](const std::string & uri) {
      if (ws.has_document(uri)) {
        return;
      }

      std::string resolved_uri = uri;

      // Try to resolve bt-dsl-pkg:// URIs to file:// URIs
      if (starts_with(uri, "bt-dsl-pkg://")) {
        auto file_uri = resolve_package_uri(uri, stdlib_base);
        if (!file_uri) {
          return;  // Cannot resolve package URI
        }
        resolved_uri = *file_uri;
      }

      const auto p = file_uri_to_path(resolved_uri);
      if (!p) {
        return;
      }
      const auto text = read_file_to_string(*p);
      if (!text) {
        return;
      }
      // Store under resolved file:// URI so go-to-definition works
      upsert_doc(resolved_uri, *text);
    };

    // Resolve a URI (potentially bt-dsl-pkg://) to a file:// URI
    auto resolve_uri = [&](const std::string & uri) -> std::string {
      if (starts_with(uri, "bt-dsl-pkg://")) {
        auto file_uri = resolve_package_uri(uri, stdlib_base);
        if (file_uri) {
          return *file_uri;
        }
      }
      return uri;  // Already a file:// URI or unresolvable
    };

    auto refresh_imports = [&](DocState & doc) {
      // Ask workspace for direct import URIs. Host then loads them.
      // Note: stdlib_uri is empty since we handle package URIs in ensure_loaded
      const json r = json::parse(ws.resolve_imports_json(doc.uri, ""));
      doc.imported_uris.clear();
      if (r.contains("uris") && r["uris"].is_array()) {
        for (const auto & u : r["uris"]) {
          if (u.is_string()) {
            // Resolve bt-dsl-pkg:// to file:// and store resolved URI
            const std::string resolved = resolve_uri(u.get<std::string>());
            doc.imported_uris.push_back(resolved);
          }
        }
      }

      for (const auto & u : doc.imported_uris) {
        ensure_loaded(u);
      }
    };

    auto publish_diagnostics = [&](const DocState & doc) {
      json dj;
      try {
        dj = json::parse(ws.diagnostics_json(doc.uri, doc.imported_uris));
      } catch (...) {
        dj = json{{"uri", doc.uri}, {"items", json::array()}};
      }

      json lsp_diags = json::array();
      if (dj.contains("items") && dj["items"].is_array()) {
        for (const auto & it : dj["items"]) {
          if (!it.is_object()) continue;
          json d0;
          d0["message"] = it.value("message", "");
          d0["severity"] = lsp_severity(it.value("severity", "Info"));
          if (it.contains("source") && it["source"].is_string()) {
            d0["source"] = it["source"];
          }
          if (it.contains("range") && it["range"].is_object()) {
            d0["range"] = to_lsp_range_from_full_range(it["range"]);
          } else {
            d0["range"] = json{
              {"start", json{{"line", 0}, {"character", 0}}},
              {"end", json{{"line", 0}, {"character", 0}}}};
          }
          lsp_diags.push_back(std::move(d0));
        }
      }

      json notif;
      notif["jsonrpc"] = "2.0";
      notif["method"] = "textDocument/publishDiagnostics";
      notif["params"] = json{{"uri", doc.uri}, {"diagnostics", lsp_diags}};
      write_message(notif);
    };

    auto pos_to_byte_offset = [&](const DocState & doc, const json & pos) -> uint32_t {
      const auto line = pos.value<uint32_t>("line", 0U);
      const auto character = pos.value<uint32_t>("character", 0U);

      if (negotiated_position_encoding == "utf-16") {
        if (auto off = utf16_position_to_byte_offset(doc, line, character)) {
          return *off;
        }
        return 0;
      }

      // Default to utf-8 (bytes).
      if (auto off = utf8_position_to_byte_offset(doc, line, character)) {
        return *off;
      }
      return 0;
    };

    auto byte_offset_to_lsp_position = [&](const DocState & doc, uint32_t byte_offset) -> json {
      const bt_dsl::SourceManager sm(doc.text);
      const auto lc = sm.get_line_column(bt_dsl::SourceLocation(byte_offset));
      const int line = std::max(0, static_cast<int>(lc.line) - 1);
      const int col = std::max(0, static_cast<int>(lc.column) - 1);
      return json{{"line", line}, {"character", col}};
    };

    auto byte_range_to_lsp_range = [&](const DocState & doc, uint32_t sb, uint32_t eb) -> json {
      return json{
        {"start", byte_offset_to_lsp_position(doc, sb)},
        {"end", byte_offset_to_lsp_position(doc, eb)},
      };
    };

    bool running = true;
    while (running) {
      const auto msg_opt = read_message();
      if (!msg_opt) {
        if (!std::cin.good()) {
          break;
        }
        continue;
      }

      const json & msg = *msg_opt;
      const std::string method = msg.value("method", "");
      const bool is_request = msg.contains("id");

      auto respond = [&](const json & id, const json & result) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = result;
        write_message(resp);
      };

      auto respond_error = [&](const json & id, int code, std::string message) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["error"] = json{{"code", code}, {"message", std::move(message)}};
        write_message(resp);
      };

      const json params = msg.value("params", json::object());

      if (method == "initialize" && is_request) {
        // Determine position encoding
        negotiated_position_encoding = "utf-8";
        try {
          if (params.contains("capabilities") && params["capabilities"].is_object()) {
            const auto & caps = params["capabilities"];
            if (caps.contains("general") && caps["general"].is_object()) {
              const auto & gen = caps["general"];
              if (gen.contains("positionEncodings") && gen["positionEncodings"].is_array()) {
                bool has_utf8 = false;
                bool has_utf16 = false;
                for (const auto & e : gen["positionEncodings"]) {
                  if (!e.is_string()) continue;
                  const auto s = e.get<std::string>();
                  if (s == "utf-8") has_utf8 = true;
                  if (s == "utf-16") has_utf16 = true;
                }
                negotiated_position_encoding = "utf-8";
                if (!has_utf8 && has_utf16) {
                  negotiated_position_encoding = "utf-16";
                }
              }
            }
          }
        } catch (...) {
          negotiated_position_encoding = "utf-8";
        }

        // Auto-detect stdlib directory using find_stdlib().
        // find_stdlib() returns the `std/` directory itself, so we use its parent
        // as the base for resolving package imports like `std/nodes.bt`.
        if (auto detected = bt_dsl::find_stdlib()) {
          stdlib_base = detected->parent_path().string();
        }

        json caps;
        caps["positionEncoding"] = negotiated_position_encoding;
        caps["textDocumentSync"] = json{{"openClose", true}, {"change", 1}};  // Full sync
        caps["completionProvider"] = json{{"resolveProvider", false}};
        caps["hoverProvider"] = true;
        caps["definitionProvider"] = true;
        caps["documentSymbolProvider"] = true;

        const json result = json{{"capabilities", caps}};
        respond(msg["id"], result);
        continue;
      }

      if (method == "initialized") {
        // no-op
        continue;
      }

      if (method == "shutdown" && is_request) {
        respond(msg["id"], json());
        continue;
      }

      if (method == "exit") {
        running = false;
        continue;
      }

      if (method == "textDocument/didOpen") {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        const std::string text = td.value("text", "");
        if (!uri.empty()) {
          auto & doc = upsert_doc(uri, text);
          refresh_imports(doc);
          publish_diagnostics(doc);
        }
        continue;
      }

      if (method == "textDocument/didChange") {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        if (uri.empty()) {
          continue;
        }

        // Full sync: take first change text
        const auto changes = params.value("contentChanges", json::array());
        if (!changes.is_array() || changes.empty()) {
          continue;
        }
        const auto & c0 = changes.at(0);
        if (!c0.is_object() || !c0.contains("text") || !c0["text"].is_string()) {
          continue;
        }

        auto & doc = upsert_doc(uri, c0["text"].get<std::string>());
        refresh_imports(doc);
        publish_diagnostics(doc);
        continue;
      }

      if (method == "textDocument/didClose") {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        if (!uri.empty()) {
          ws.remove_document(uri);
          docs.erase(uri);

          // Clear diagnostics on close
          json notif;
          notif["jsonrpc"] = "2.0";
          notif["method"] = "textDocument/publishDiagnostics";
          notif["params"] = json{{"uri", uri}, {"diagnostics", json::array()}};
          write_message(notif);
        }
        continue;
      }

      if (method == "textDocument/completion" && is_request) {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        const auto pos = params.value("position", json::object());
        auto it = docs.find(uri);
        if (it == docs.end()) {
          respond(msg["id"], json{{"isIncomplete", false}, {"items", json::array()}});
          continue;
        }
        const DocState & doc = it->second;
        const uint32_t off = pos_to_byte_offset(doc, pos);

        json cj;
        try {
          cj = json::parse(ws.completion_json(uri, off, doc.imported_uris));
        } catch (...) {
          cj = json{{"isIncomplete", false}, {"items", json::array()}};
        }

        json items = json::array();
        if (cj.contains("items") && cj["items"].is_array()) {
          for (const auto & it0 : cj["items"]) {
            if (!it0.is_object()) continue;
            const std::string label = it0.value("label", "");
            const std::string kind = it0.value("kind", "Text");
            const std::string detail = it0.value("detail", "");
            const std::string insert = it0.value("insertText", label);

            json item;
            item["label"] = label;
            item["kind"] = completion_kind(kind);
            if (!detail.empty()) {
              item["detail"] = detail;
            }

            // textEdit: replaceRange is in bytes (absolute)
            if (it0.contains("replaceRange") && it0["replaceRange"].is_object()) {
              const auto sb = it0["replaceRange"].value<uint32_t>("startByte", 0U);
              const auto eb = it0["replaceRange"].value<uint32_t>("endByte", 0U);
              item["textEdit"] = json{
                {"range", byte_range_to_lsp_range(doc, sb, eb)},
                {"newText", insert},
              };
            } else {
              item["insertText"] = insert;
            }

            items.push_back(std::move(item));
          }
        }

        const json result =
          json{{"isIncomplete", cj.value("isIncomplete", false)}, {"items", items}};
        respond(msg["id"], result);
        continue;
      }

      if (method == "textDocument/hover" && is_request) {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        const auto pos = params.value("position", json::object());
        auto it = docs.find(uri);
        if (it == docs.end()) {
          respond(msg["id"], nullptr);
          continue;
        }
        const DocState & doc = it->second;
        const uint32_t off = pos_to_byte_offset(doc, pos);

        json hj;
        try {
          hj = json::parse(ws.hover_json(uri, off, doc.imported_uris));
        } catch (...) {
          hj = json::object();
        }

        if (
          !hj.contains("contents") || !hj["contents"].is_string() ||
          hj["contents"].get<std::string>().empty()) {
          respond(msg["id"], nullptr);
          continue;
        }

        json out;
        out["contents"] = json{{"kind", "markdown"}, {"value", hj["contents"]}};
        if (hj.contains("range") && hj["range"].is_object()) {
          out["range"] = to_lsp_range_from_full_range(hj["range"]);
        }

        respond(msg["id"], out);
        continue;
      }

      if (method == "textDocument/definition" && is_request) {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");
        const auto pos = params.value("position", json::object());
        auto it = docs.find(uri);
        if (it == docs.end()) {
          respond(msg["id"], json::array());
          continue;
        }
        const DocState & doc = it->second;
        const uint32_t off = pos_to_byte_offset(doc, pos);

        json dj;
        try {
          dj = json::parse(ws.definition_json(uri, off, doc.imported_uris));
        } catch (...) {
          dj = json{{"locations", json::array()}};
        }

        json locs = json::array();
        if (dj.contains("locations") && dj["locations"].is_array()) {
          for (const auto & loc : dj["locations"]) {
            if (!loc.is_object()) continue;
            json out;
            out["uri"] = loc.value("uri", "");
            if (loc.contains("range") && loc["range"].is_object()) {
              out["range"] = to_lsp_range_from_full_range(loc["range"]);
            } else {
              out["range"] = json{
                {"start", json{{"line", 0}, {"character", 0}}},
                {"end", json{{"line", 0}, {"character", 0}}}};
            }
            locs.push_back(std::move(out));
          }
        }

        respond(msg["id"], locs);
        continue;
      }

      if (method == "textDocument/documentSymbol" && is_request) {
        const auto td = params.value("textDocument", json::object());
        const std::string uri = td.value("uri", "");

        json sj;
        try {
          sj = json::parse(ws.document_symbols_json(uri));
        } catch (...) {
          sj = json{{"symbols", json::array()}};
        }

        json out = json::array();
        if (sj.contains("symbols") && sj["symbols"].is_array()) {
          for (const auto & s0 : sj["symbols"]) {
            if (!s0.is_object()) continue;
            json ds;
            ds["name"] = s0.value("name", "");
            ds["kind"] = symbol_kind(s0.value("kind", ""));
            if (s0.contains("range") && s0["range"].is_object()) {
              ds["range"] = to_lsp_range_from_full_range(s0["range"]);
              ds["selectionRange"] = to_lsp_range_from_full_range(s0["selectionRange"]);
            } else {
              ds["range"] = json{
                {"start", json{{"line", 0}, {"character", 0}}},
                {"end", json{{"line", 0}, {"character", 0}}}};
              ds["selectionRange"] = ds["range"];
            }
            ds["children"] = json::array();
            out.push_back(std::move(ds));
          }
        }

        respond(msg["id"], out);
        continue;
      }

      // Unknown method
      if (is_request) {
        respond_error(msg["id"], -32601, "Method not found");
      }
    }

    return 0;
  } catch (const std::exception & e) {
    std::cerr << "bt_dsl_lsp_server: fatal error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "bt_dsl_lsp_server: fatal unknown error\n";
    return 1;
  }
}
