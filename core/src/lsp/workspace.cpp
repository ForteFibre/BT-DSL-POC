#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/core/symbol_table.hpp"
#include "bt_dsl/lsp/completion_context.hpp"
#include "bt_dsl/lsp/lsp.hpp"
#include "bt_dsl/parser/parser.hpp"
#include "bt_dsl/semantic/analyzer.hpp"
#include "bt_dsl/semantic/node_registry.hpp"
#include "bt_dsl/semantic/type_system.hpp"
#include "bt_dsl/syntax/keywords.hpp"

namespace bt_dsl::lsp
{
namespace
{

using json = nlohmann::json;

bool contains_byte(const bt_dsl::SourceRange & r, uint32_t byte)
{
  // Treat SourceRange as a half-open interval [start, end)
  return r.start_byte <= byte && byte < r.end_byte;
}

uint32_t clamp_byte_offset(uint32_t off, size_t text_size)
{
  if (off > text_size) {
    return static_cast<uint32_t>(text_size);
  }
  return off;
}

bool is_ident_char(unsigned char c) { return std::isalnum(c) || c == '_'; }

struct WordRange
{
  uint32_t startByte = 0;
  uint32_t endByte = 0;
};

struct ByteRange
{
  uint32_t startByte = 0;
  uint32_t endByte = 0;
};

WordRange word_range_at(std::string_view text, uint32_t byte_offset)
{
  WordRange r;
  const auto size = static_cast<uint32_t>(text.size());
  if (size == 0) {
    r.startByte = r.endByte = 0;
    return r;
  }

  byte_offset = clamp_byte_offset(byte_offset, size);
  uint32_t pos = byte_offset;

  // If we're at a boundary (including EOF) and the previous char is an ident,
  // treat it as being on that word.
  if (
    pos > 0 && (pos == size || !is_ident_char(static_cast<unsigned char>(text[pos]))) &&
    is_ident_char(static_cast<unsigned char>(text[pos - 1]))) {
    pos -= 1;
  }

  if (pos >= size || !is_ident_char(static_cast<unsigned char>(text[pos]))) {
    r.startByte = r.endByte = byte_offset;
    return r;
  }

  uint32_t start = pos;
  while (start > 0 && is_ident_char(static_cast<unsigned char>(text[start - 1]))) {
    start -= 1;
  }

  uint32_t end = pos + 1;
  while (end < size && is_ident_char(static_cast<unsigned char>(text[end]))) {
    end += 1;
  }

  r.startByte = start;
  r.endByte = end;
  return r;
}

ByteRange completion_replace_range_at(std::string_view text, uint32_t byte_offset)
{
  ByteRange r;
  byte_offset = clamp_byte_offset(byte_offset, text.size());

  // If the cursor is on whitespace, use insertion range to avoid replacing the previous token.
  if (byte_offset < text.size() && std::isspace(static_cast<unsigned char>(text[byte_offset]))) {
    r.startByte = r.endByte = byte_offset;
    return r;
  }

  const auto w = word_range_at(text, byte_offset);
  if (w.endByte > w.startByte) {
    r.startByte = w.startByte;
    r.endByte = w.endByte;
  } else {
    r.startByte = r.endByte = byte_offset;
  }
  return r;
}

std::optional<std::string> word_at(std::string_view text, uint32_t byte_offset)
{
  const auto r = word_range_at(text, byte_offset);
  if (r.endByte <= r.startByte || r.endByte > text.size()) {
    return std::nullopt;
  }
  return std::string(text.substr(r.startByte, r.endByte - r.startByte));
}

json range_to_json(const bt_dsl::SourceRange & r)
{
  json j;
  j["startByte"] = r.start_byte;
  j["endByte"] = r.end_byte;
  j["startLine"] = r.start_line;
  j["startColumn"] = r.start_column;
  j["endLine"] = r.end_line;
  j["endColumn"] = r.end_column;
  return j;
}

json byte_range_to_json(const ByteRange & r)
{
  json j;
  j["startByte"] = r.startByte;
  j["endByte"] = r.endByte;
  return j;
}

bt_dsl::SourceRange narrow_to_identifier(
  std::string_view text, const bt_dsl::SourceRange & decl_range, std::string_view ident)
{
  bt_dsl::SourceRange r = decl_range;

  const uint32_t start =
    std::min<uint32_t>(decl_range.start_byte, static_cast<uint32_t>(text.size()));
  const uint32_t end = std::min<uint32_t>(decl_range.end_byte, static_cast<uint32_t>(text.size()));
  if (end <= start) {
    return r;
  }

  const std::string_view slice = text.substr(start, end - start);
  const size_t pos = slice.find(ident);
  if (pos == std::string_view::npos) {
    return r;
  }

  r.start_byte = start + static_cast<uint32_t>(pos);
  r.end_byte = r.start_byte + static_cast<uint32_t>(ident.size());
  // Line/column are left as-is; host is expected to use byte offsets.
  return r;
}

std::string severity_to_string(bt_dsl::Severity s)
{
  switch (s) {
    case bt_dsl::Severity::Error:
      return "Error";
    case bt_dsl::Severity::Warning:
      return "Warning";
    case bt_dsl::Severity::Info:
      return "Info";
    case bt_dsl::Severity::Hint:
      return "Hint";
  }
  return "Error";
}

std::string parse_severity_to_string(bt_dsl::ParseError::Severity s)
{
  switch (s) {
    case bt_dsl::ParseError::Severity::Error:
      return "Error";
    case bt_dsl::ParseError::Severity::Warning:
      return "Warning";
    case bt_dsl::ParseError::Severity::Info:
      return "Info";
  }
  return "Error";
}

struct AstHit
{
  const bt_dsl::TreeDef * tree = nullptr;
  const bt_dsl::NodeStmt * node = nullptr;
  const bt_dsl::InlineBlackboardDecl * inline_decl = nullptr;
  const bt_dsl::VarRef * var_ref = nullptr;
};

void consider_node_hit(
  const bt_dsl::NodeStmt & node, uint32_t off, const bt_dsl::TreeDef & tree, AstHit & hit)
{
  if (!contains_byte(node.range, off)) {
    return;
  }
  if (!hit.node) {
    hit.node = &node;
    hit.tree = &tree;
    return;
  }

  const uint32_t cur_len = hit.node->range.end_byte - hit.node->range.start_byte;
  const uint32_t new_len = node.range.end_byte - node.range.start_byte;
  if (new_len < cur_len) {
    hit.node = &node;
    hit.tree = &tree;
  }
}

void consider_inline_decl_hit(const bt_dsl::InlineBlackboardDecl & decl, uint32_t off, AstHit & hit)
{
  if (!contains_byte(decl.range, off)) {
    return;
  }
  if (!hit.inline_decl) {
    hit.inline_decl = &decl;
    return;
  }

  const uint32_t cur_len = hit.inline_decl->range.end_byte - hit.inline_decl->range.start_byte;
  const uint32_t new_len = decl.range.end_byte - decl.range.start_byte;
  if (new_len < cur_len) {
    hit.inline_decl = &decl;
  }
}

void consider_varref_hit(const bt_dsl::VarRef & ref, uint32_t off, AstHit & hit)
{
  if (!contains_byte(ref.range, off)) {
    return;
  }
  if (!hit.var_ref) {
    hit.var_ref = &ref;
    return;
  }
  const uint32_t cur_len = hit.var_ref->range.end_byte - hit.var_ref->range.start_byte;
  const uint32_t new_len = ref.range.end_byte - ref.range.start_byte;
  if (new_len < cur_len) {
    hit.var_ref = &ref;
  }
}

void visit_expression(const bt_dsl::Expression & e, uint32_t off, AstHit & hit);

void visit_expression(const bt_dsl::Expression & e, uint32_t off, AstHit & hit)
{
  std::visit(
    [&](const auto & node) {
      using T = std::decay_t<decltype(node)>;
      if constexpr (std::is_same_v<T, bt_dsl::Literal>) {
        (void)node;
      } else if constexpr (std::is_same_v<T, bt_dsl::VarRef>) {
        consider_varref_hit(node, off, hit);
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::BinaryExpr>>) {
        if (contains_byte(node->range, off)) {
          visit_expression(node->left, off, hit);
          visit_expression(node->right, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::UnaryExpr>>) {
        if (contains_byte(node->range, off)) {
          visit_expression(node->operand, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::CastExpr>>) {
        if (contains_byte(node->range, off)) {
          visit_expression(node->expr, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::IndexExpr>>) {
        if (contains_byte(node->range, off)) {
          visit_expression(node->base, off, hit);
          visit_expression(node->index, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::ArrayLiteralExpr>>) {
        if (contains_byte(node->range, off)) {
          for (const auto & el : node->elements) {
            visit_expression(el, off, hit);
          }
          if (node->repeat_value) {
            visit_expression(*node->repeat_value, off, hit);
          }
          if (node->repeat_count) {
            visit_expression(*node->repeat_count, off, hit);
          }
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::VecMacroExpr>>) {
        if (contains_byte(node->range, off)) {
          for (const auto & el : node->value.elements) {
            visit_expression(el, off, hit);
          }
          if (node->value.repeat_value) {
            visit_expression(*node->value.repeat_value, off, hit);
          }
          if (node->value.repeat_count) {
            visit_expression(*node->value.repeat_count, off, hit);
          }
        }
      }
    },
    e);
}

void visit_argument_value(const bt_dsl::ArgumentValue & v, uint32_t off, AstHit & hit)
{
  std::visit(
    [&](const auto & av) {
      using T = std::decay_t<decltype(av)>;
      if constexpr (std::is_same_v<T, bt_dsl::Expression>) {
        visit_expression(av, off, hit);
      } else if constexpr (std::is_same_v<T, bt_dsl::InlineBlackboardDecl>) {
        consider_inline_decl_hit(av, off, hit);
      }
    },
    v);
}

void visit_node_stmt(
  const bt_dsl::NodeStmt & node, uint32_t off, const bt_dsl::TreeDef & tree, AstHit & hit);

void visit_statement(
  const bt_dsl::Statement & s, uint32_t off, const bt_dsl::TreeDef & tree, AstHit & hit)
{
  std::visit(
    [&](const auto & elem) {
      using T = std::decay_t<decltype(elem)>;
      if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::NodeStmt>>) {
        visit_node_stmt(*elem, off, tree, hit);
      } else if constexpr (std::is_same_v<T, bt_dsl::AssignmentStmt>) {
        if (contains_byte(elem.range, off)) {
          for (const auto & idx : elem.indices) {
            visit_expression(idx, off, hit);
          }
          visit_expression(elem.value, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::BlackboardDeclStmt>) {
        if (elem.initial_value && contains_byte(elem.range, off)) {
          visit_expression(*elem.initial_value, off, hit);
        }
      } else if constexpr (std::is_same_v<T, bt_dsl::ConstDeclStmt>) {
        if (contains_byte(elem.range, off)) {
          visit_expression(elem.value, off, hit);
        }
      }
    },
    s);
}

void visit_node_stmt(
  const bt_dsl::NodeStmt & node, uint32_t off, const bt_dsl::TreeDef & tree, AstHit & hit)
{
  consider_node_hit(node, off, tree, hit);
  if (!contains_byte(node.range, off)) {
    return;
  }

  for (const auto & pc : node.preconditions) {
    if (contains_byte(pc.range, off)) {
      visit_expression(pc.condition, off, hit);
    }
  }

  for (const auto & arg : node.args) {
    if (contains_byte(arg.range, off)) {
      visit_argument_value(arg.value, off, hit);
    }
  }

  for (const auto & child : node.children) {
    const bool child_contains = std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::NodeStmt>>) {
          return contains_byte(elem->range, off);
        } else {
          return contains_byte(elem.range, off);
        }
      },
      child);
    if (child_contains) {
      visit_statement(child, off, tree, hit);
    }
  }
}

std::optional<const bt_dsl::TreeDef *> find_tree_at(const bt_dsl::Program & p, uint32_t off)
{
  const bt_dsl::TreeDef * best = nullptr;
  for (const auto & t : p.trees) {
    if (!contains_byte(t.range, off)) {
      continue;
    }
    if (!best) {
      best = &t;
      continue;
    }
    const uint32_t cur_len = best->range.end_byte - best->range.start_byte;
    const uint32_t new_len = t.range.end_byte - t.range.start_byte;
    if (new_len < cur_len) {
      best = &t;
    }
  }
  if (!best) {
    return std::nullopt;
  }
  return best;
}

AstHit find_ast_hit(const bt_dsl::Program & p, uint32_t off)
{
  AstHit hit;
  const auto tree_opt = find_tree_at(p, off);
  if (!tree_opt || !*tree_opt) {
    return hit;
  }
  hit.tree = *tree_opt;

  for (const auto & stmt : hit.tree->body) {
    const bool contains = std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::NodeStmt>>) {
          return contains_byte(elem->range, off);
        } else {
          return contains_byte(elem.range, off);
        }
      },
      stmt);
    if (contains) {
      visit_statement(stmt, off, *hit.tree, hit);
    }
  }

  return hit;
}

bool is_on_node_name(std::string_view text, const bt_dsl::NodeStmt & node, uint32_t off)
{
  const auto r = word_range_at(text, off);
  if (r.endByte <= r.startByte || r.endByte > text.size()) {
    return false;
  }
  const std::string_view w = text.substr(r.startByte, r.endByte - r.startByte);
  if (w != node.node_name) {
    return false;
  }

  // Be tolerant of indentation / formatting differences: as long as the word
  // is inside the node statement range, treat it as the node name.
  return node.range.start_byte <= r.startByte && r.endByte <= node.range.end_byte;
}

std::vector<std::string> builtin_node_candidates()
{
  return {
    "Sequence", "Fallback", "Parallel", "ReactiveSequence", "ReactiveFallback",
  };
}

std::string format_port(const bt_dsl::PortInfo & p)
{
  std::string out;
  out += std::string(bt_dsl::to_string(p.direction));
  out += " ";
  out += p.name;
  if (p.type_name) {
    out += ": ";
    out += *p.type_name;
  }
  return out;
}

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
  // Require an explicit extension ("./foo.bt" ok, "./foo" not).
  const size_t last_slash = spec.find_last_of('/');
  const size_t name_start = (last_slash == std::string_view::npos) ? 0 : (last_slash + 1);
  const std::string_view name = spec.substr(name_start);
  if (name.empty()) return false;
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos) return false;
  if (dot == name.size() - 1) return false;
  return true;
}

std::string package_import_uri(std::string_view spec)
{
  // Host-defined mapping. We use a stable synthetic URI so the host can
  // provide document contents via Workspace::set_document().
  // Example: import "my_pkg/foo.bt" -> bt-dsl-pkg://my_pkg/foo.bt
  std::string out = "bt-dsl-pkg://";
  out.append(spec.data(), spec.size());
  return out;
}

// Remove dot-segments from a URI path, per RFC 3986 (simplified).
// Input and output are expected to use '/' separators.
std::string remove_dot_segments(std::string_view path)
{
  std::vector<std::string_view> segs;
  segs.reserve(32);

  size_t i = 0;
  while (i <= path.size()) {
    const size_t j = path.find('/', i);
    const size_t end = (j == std::string_view::npos) ? path.size() : j;
    const std::string_view seg = path.substr(i, end - i);
    if (seg == "..") {
      if (!segs.empty()) {
        segs.pop_back();
      }
    } else if (!seg.empty() && seg != ".") {
      segs.push_back(seg);
    }

    if (j == std::string_view::npos) {
      break;
    }
    i = j + 1;
  }

  std::string out;
  if (starts_with(path, "/")) {
    out.push_back('/');
  }
  for (size_t k = 0; k < segs.size(); ++k) {
    if (k > 0) {
      out.push_back('/');
    }
    out.append(segs[k].data(), segs[k].size());
  }
  return out;
}

std::optional<std::string> resolve_relative_import_uri(
  std::string_view from_uri, std::string_view spec)
{
  // Policy: only relative imports are supported.
  if (!is_relative_import_spec(spec)) {
    return std::nullopt;
  }

  // Only resolve against file:// URIs.
  constexpr std::string_view file_prefix = "file://";
  if (!starts_with(from_uri, file_prefix)) {
    return std::nullopt;
  }

  const size_t last_slash = from_uri.find_last_of('/');
  if (last_slash == std::string_view::npos || last_slash + 1 <= file_prefix.size()) {
    return std::nullopt;
  }

  // Directory URI, including trailing '/'.
  const std::string_view dir_uri = from_uri.substr(0, last_slash + 1);
  std::string combined = std::string(dir_uri);
  combined += std::string(spec);

  // Normalize only the path component.
  const std::string_view combined_sv = combined;
  const std::string_view path_part = combined_sv.substr(file_prefix.size());
  if (path_part.empty() || path_part[0] != '/') {
    return std::nullopt;
  }

  const std::string normalized_path = remove_dot_segments(path_part);
  std::string out;
  out.reserve(file_prefix.size() + normalized_path.size());
  out.append(file_prefix.data(), file_prefix.size());
  out += normalized_path;
  return out;
}

}  // namespace

// =============================================================================
// Workspace::Impl
// =============================================================================

struct Workspace::Impl
{
  bt_dsl::Parser parser;
  bt_dsl::Analyzer analyzer;

  struct Document
  {
    std::string uri;
    std::string text;

    bool parsed = false;
    bt_dsl::Program program{};
    std::vector<bt_dsl::ParseError> parse_errors;
  };

  std::unordered_map<std::string, Document> docs;

  Document * get_doc(std::string_view uri)
  {
    auto it = docs.find(std::string(uri));
    if (it == docs.end()) {
      return nullptr;
    }
    return &it->second;
  }

  const Document * get_doc(std::string_view uri) const
  {
    auto it = docs.find(std::string(uri));
    if (it == docs.end()) {
      return nullptr;
    }
    return &it->second;
  }

  void ensure_parsed(Document & d)
  {
    if (d.parsed) {
      return;
    }

    auto [program, errors] = parser.parse_with_recovery(d.text);
    d.program = std::move(program);
    d.parse_errors = std::move(errors);
    d.parsed = true;
  }

  std::vector<const bt_dsl::Program *> imported_program_ptrs(
    const std::vector<std::string> & imported_uris)
  {
    std::vector<const bt_dsl::Program *> out;
    out.reserve(imported_uris.size());

    for (const auto & u : imported_uris) {
      auto it = docs.find(u);
      if (it == docs.end()) {
        continue;
      }
      ensure_parsed(it->second);
      out.push_back(&it->second.program);
    }

    return out;
  }

  bt_dsl::AnalysisResult analyze_with_imports(
    std::string_view uri, const std::vector<std::string> & imported_uris)
  {
    auto * doc = get_doc(uri);
    if (!doc) {
      return bt_dsl::AnalysisResult{};
    }

    ensure_parsed(*doc);

    auto imported_ptrs = imported_program_ptrs(imported_uris);
    if (imported_ptrs.empty()) {
      return bt_dsl::Analyzer::analyze(doc->program);
    }

    return bt_dsl::Analyzer::analyze(doc->program, imported_ptrs);
  }

  struct DefinitionLocation
  {
    std::string uri;
    const Document * doc = nullptr;
    bt_dsl::SourceRange range{};
  };

  std::optional<DefinitionLocation> find_node_declaration_location(
    const Document & current, const std::vector<std::string> & imported_uris,
    std::string_view node_name)
  {
    // Prefer same-document declaration.
    for (const auto & d : current.program.declarations) {
      if (d.name == node_name) {
        return DefinitionLocation{current.uri, &current, d.range};
      }
    }

    // Then same-document tree definition (subtree).
    for (const auto & t : current.program.trees) {
      if (t.name == node_name) {
        return DefinitionLocation{current.uri, &current, t.range};
      }
    }

    // Then search imported documents.
    for (const auto & u : imported_uris) {
      // ensure_parsed requires non-const; re-fetch non-const document.
      auto * mutable_doc = get_doc(u);
      if (!mutable_doc) {
        continue;
      }
      ensure_parsed(*mutable_doc);
      for (const auto & d : mutable_doc->program.declarations) {
        if (d.name == node_name) {
          return DefinitionLocation{mutable_doc->uri, mutable_doc, d.range};
        }
      }

      for (const auto & t : mutable_doc->program.trees) {
        if (t.name == node_name) {
          return DefinitionLocation{mutable_doc->uri, mutable_doc, t.range};
        }
      }
    }

    return std::nullopt;
  }

  json diagnostics_json_impl(std::string_view uri, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["items"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }

    ensure_parsed(*doc);

    // Import resolution diagnostics (policy + best-effort missing-doc check)
    for (const auto & imp : doc->program.imports) {
      const std::string_view spec = imp.path;

      // Reference constraints: absolute path ban + extension required.
      if (starts_with(spec, "/")) {
        json item;
        item["source"] = "import";
        item["message"] = "Absolute import paths are not allowed: \"" + std::string(spec) + "\"";
        item["severity"] = "Error";
        item["range"] = range_to_json(imp.range);
        out["items"].push_back(std::move(item));
        continue;
      }
      if (!has_required_extension(spec)) {
        json item;
        item["source"] = "import";
        item["message"] = "Import path must include an extension: \"" + std::string(spec) + "\"";
        item["severity"] = "Error";
        item["range"] = range_to_json(imp.range);
        out["items"].push_back(std::move(item));
        continue;
      }

      // Relative imports: resolve against file:// URIs.
      if (is_relative_import_spec(spec)) {
        auto resolved = resolve_relative_import_uri(doc->uri, spec);
        if (!resolved) {
          json item;
          item["source"] = "import";
          item["message"] = "Cannot resolve relative import against this document URI";
          item["severity"] = "Error";
          item["range"] = range_to_json(imp.range);
          out["items"].push_back(std::move(item));
          continue;
        }

        if (docs.find(*resolved) == docs.end()) {
          json item;
          item["source"] = "import";
          item["message"] = "Imported document is not loaded: \"" + std::string(spec) + "\"";
          item["severity"] = "Error";
          item["range"] = range_to_json(imp.range);
          out["items"].push_back(std::move(item));
        }
        continue;
      }

      // Package imports: implementation-defined; in this serverless workspace
      // they must be provided by the host via set_document() using the
      // synthetic bt-dsl-pkg:// URI form.
      const std::string pkg_uri = package_import_uri(spec);
      if (docs.find(pkg_uri) == docs.end()) {
        json item;
        item["source"] = "import";
        item["message"] =
          "Cannot resolve package import (host must provide it): \"" + std::string(spec) + "\"";
        item["severity"] = "Error";
        item["range"] = range_to_json(imp.range);
        out["items"].push_back(std::move(item));
      }
    }

    // Parse errors
    for (const auto & e : doc->parse_errors) {
      json item;
      item["source"] = "parser";
      item["message"] = e.message;
      item["severity"] = parse_severity_to_string(e.severity);
      item["range"] = range_to_json(e.range);
      out["items"].push_back(std::move(item));
    }

    // Semantic diagnostics
    // If the document has parse errors, avoid emitting analyzer diagnostics.
    // Parser recovery may insert placeholder expressions (e.g. 0/true), which
    // can easily lead to misleading semantic errors in an incomplete AST.
    const bool has_parse_error = std::any_of(
      doc->parse_errors.begin(), doc->parse_errors.end(),
      [](const ParseError & e) { return e.severity == ParseError::Severity::Error; });

    if (!has_parse_error) {
      auto analysis = analyze_with_imports(uri, imported_uris);
      for (const auto & d : analysis.diagnostics.all()) {
        json item;
        item["source"] = "analyzer";
        item["message"] = d.message;
        item["severity"] = severity_to_string(d.severity);
        if (!d.code.empty()) {
          item["code"] = d.code;
        }
        item["range"] = range_to_json(d.range);
        out["items"].push_back(std::move(item));
      }
    }

    return out;
  }

  json resolve_imports_json_impl(std::string_view uri, std::string_view stdlib_uri)
  {
    json out;
    out["uri"] = std::string(uri);
    out["stdlibUri"] = std::string(stdlib_uri);
    out["uris"] = json::array();

    // Spec: import is non-transitive. Return only direct imports of the
    // requested document (plus optional implicit stdlib).
    std::unordered_set<std::string> seen;

    auto enqueue = [&](const std::string & u) {
      if (u.empty()) {
        return;
      }
      if (u == std::string(uri)) {
        return;
      }
      if (!seen.insert(u).second) {
        return;
      }
      out["uris"].push_back(u);
    };

    if (!stdlib_uri.empty()) {
      enqueue(std::string(stdlib_uri));
    }

    // Add direct imports.
    auto * root = get_doc(uri);
    if (root) {
      ensure_parsed(*root);
      for (const auto & imp : root->program.imports) {
        if (is_relative_import_spec(imp.path)) {
          if (auto resolved = resolve_relative_import_uri(root->uri, imp.path)) {
            enqueue(*resolved);
          }
        } else {
          enqueue(package_import_uri(imp.path));
        }
      }
    }

    return out;
  }

  json completion_json_impl(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris,
    std::string_view trigger)
  {
    (void)trigger;
    json out;
    out["uri"] = std::string(uri);
    out["isIncomplete"] = false;
    out["items"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    const auto analysis = analyze_with_imports(uri, imported_uris);
    const auto replace_range = completion_replace_range_at(doc->text, byte_offset);

    // Completion context is determined strictly from CST (tree-sitter).
    // No brace/text/AST fallback: candidate types are gated by syntactic region.
    const auto ctx_opt = classify_completion_context(doc->text, byte_offset);
    if (!ctx_opt) {
      return out;
    }
    const auto & ctx = *ctx_opt;

    auto push_item =
      [&](std::string label, std::string kind, std::string detail, std::string insert) {
        json item;
        item["label"] = std::move(label);
        item["kind"] = std::move(kind);
        if (!detail.empty()) {
          item["detail"] = std::move(detail);
        }
        item["insertText"] = std::move(insert);
        item["replaceRange"] = byte_range_to_json(replace_range);
        out["items"].push_back(std::move(item));
      };

    auto push_directions = [&]() {
      for (const auto & d : bt_dsl::syntax::k_port_directions) {
        push_item(std::string(d), "Keyword", "direction", std::string(d) + " ");
      }
    };

    auto push_visible_vars = [&]() {
      const bt_dsl::Scope * scope = nullptr;
      if (ctx.tree_name) {
        scope = analysis.symbols.tree_scope(*ctx.tree_name);
      }
      if (!scope) {
        scope = analysis.symbols.global_scope();
      }

      std::unordered_set<std::string> seen;
      for (const bt_dsl::Scope * s = scope; s != nullptr; s = s->parent()) {
        for (const auto & [name, sym] : s->symbols()) {
          if (!seen.insert(name).second) {
            continue;
          }
          std::string detail;
          if (sym.type_name) {
            detail = *sym.type_name;
          }
          push_item(sym.name, "Variable", detail, sym.name);
        }
      }
    };

    auto push_callable_ports = [&](bool include_colon) {
      if (!ctx.callable_name) {
        return;
      }
      const bt_dsl::NodeInfo * info = analysis.nodes.get_node(*ctx.callable_name);
      if (!info) {
        return;
      }
      for (const auto & p : info->ports) {
        std::string detail;
        if (p.type_name) {
          detail = *p.type_name;
        }
        const std::string insert = include_colon ? (p.name + ": ") : p.name;
        push_item(p.name, "Port", detail, insert);
      }
    };

    if (ctx.kind == CompletionContextKind::ImportPath) {
      // Path completion is intentionally not provided here.
      return out;
    }

    if (ctx.kind == CompletionContextKind::TopLevelKeywords) {
      for (const auto & kw : bt_dsl::syntax::k_top_level_keywords) {
        push_item(std::string(kw), "Keyword", "keyword", std::string(kw) + " ");
      }
      return out;
    }

    if (ctx.kind == CompletionContextKind::PreconditionKind) {
      for (const auto & k : bt_dsl::syntax::k_precondition_kinds) {
        // Cursor is after '@' (or within the kind identifier). Suggest kind + '('.
        push_item(std::string(k), "Keyword", "precondition", std::string(k) + "(");
      }
      return out;
    }

    if (ctx.kind == CompletionContextKind::PortDirection) {
      push_directions();
      return out;
    }

    if (
      ctx.kind == CompletionContextKind::ArgStart || ctx.kind == CompletionContextKind::ArgName ||
      ctx.kind == CompletionContextKind::ArgValue ||
      ctx.kind == CompletionContextKind::BlackboardRefName) {
      // Argument contexts:
      // - ArgName: ports only (named argument key).
      // - ArgValue/BlackboardRefName: directions + visible symbols.
      // - ArgStart: both ports and values are plausible (named or positional).
      if (
        ctx.kind == CompletionContextKind::ArgStart || ctx.kind == CompletionContextKind::ArgName) {
        push_callable_ports(true);
      }
      if (
        ctx.kind == CompletionContextKind::ArgStart ||
        ctx.kind == CompletionContextKind::ArgValue ||
        ctx.kind == CompletionContextKind::BlackboardRefName) {
        push_directions();
        push_visible_vars();
      }
      return out;
    }

    // Node name contexts inside tree bodies.
    const bool want_nodes =
      (ctx.kind == CompletionContextKind::TreeBody || ctx.kind == CompletionContextKind::NodeName);
    if (!want_nodes) {
      return out;
    }

    {
      std::vector<std::string> names = analysis.nodes.all_node_names();
      auto builtins = builtin_node_candidates();
      names.insert(names.end(), builtins.begin(), builtins.end());

      std::sort(names.begin(), names.end());
      names.erase(std::unique(names.begin(), names.end()), names.end());

      for (const auto & n : names) {
        const bt_dsl::NodeInfo * info = analysis.nodes.get_node(n);
        if (want_nodes) {
          if (info && info->category == bt_dsl::NodeCategory::Decorator) {
            continue;
          }
        }

        std::string detail;
        if (info) {
          detail = std::string(bt_dsl::node_category_to_string(info->category));
        }
        push_item(n, "Node", detail, n);
      }
    }

    return out;
  }

  json hover_json_impl(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["contents"] = nullptr;
    out["range"] = nullptr;

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    const auto analysis = analyze_with_imports(uri, imported_uris);
    const auto hit = find_ast_hit(doc->program, byte_offset);

    // Variable hover (VarRef + inline out var decl)
    if (hit.tree && (hit.var_ref || hit.inline_decl)) {
      const std::string name = hit.var_ref ? hit.var_ref->name : hit.inline_decl->name;
      const bt_dsl::SourceRange r = hit.var_ref ? hit.var_ref->range : hit.inline_decl->range;

      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      const bt_dsl::Symbol * sym = analysis.symbols.resolve(name, scope);

      std::string md;
      md += "**" + name + "**";
      if (sym) {
        std::optional<std::string> type_str;
        if (sym->type_name) {
          type_str = *sym->type_name;
        } else if (hit.tree) {
          if (const auto * ctx = analysis.get_tree_context(hit.tree->name)) {
            if (const auto * t = ctx->get_type(sym->name)) {
              type_str = t->to_string();
            }
          }
        }
        if (type_str) {
          md += "\n\nType: `" + *type_str + "`";
        }
      }

      out["contents"] = md;
      out["range"] = range_to_json(r);
      return out;
    }

    // Node hover (including decorators and subtree calls)
    if (auto w = word_at(doc->text, byte_offset)) {
      const bt_dsl::NodeInfo * info = analysis.nodes.get_node(*w);
      if (info) {
        std::string md;
        md += "**" + *w + "**";
        md +=
          "\n\nCategory: `" + std::string(bt_dsl::node_category_to_string(info->category)) + "`";

        if (!info->ports.empty()) {
          md += "\n\nPorts:";
          for (const auto & p : info->ports) {
            md += "\n- `" + format_port(p) + "`";
          }
        }

        out["contents"] = md;
        // Selection range = current word
        const auto r = word_range_at(doc->text, byte_offset);
        bt_dsl::SourceRange sr;
        sr.start_byte = r.startByte;
        sr.end_byte = r.endByte;
        out["range"] = range_to_json(sr);
        return out;
      }
    }

    return out;
  }

  json definition_json_impl(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["locations"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    const auto analysis = analyze_with_imports(uri, imported_uris);
    const auto hit = find_ast_hit(doc->program, byte_offset);

    // Import path definition: jump to the imported file.
    for (const auto & imp : doc->program.imports) {
      const auto narrowed = narrow_to_identifier(doc->text, imp.range, imp.path);
      if (!contains_byte(narrowed, byte_offset)) {
        continue;
      }
      if (auto resolved = resolve_relative_import_uri(doc->uri, imp.path)) {
        json loc;
        loc["uri"] = *resolved;
        bt_dsl::SourceRange zr;
        zr.start_byte = 0;
        zr.end_byte = 0;
        loc["range"] = range_to_json(zr);
        out["locations"].push_back(std::move(loc));
        return out;
      }
    }

    auto push_loc = [&](
                      const std::string & target_uri, const std::string & target_text,
                      const bt_dsl::SourceRange & r, std::string_view identifier) {
      json loc;
      loc["uri"] = target_uri;
      loc["range"] = range_to_json(narrow_to_identifier(target_text, r, std::string(identifier)));
      out["locations"].push_back(std::move(loc));
    };

    auto push_loc_same_doc = [&](const bt_dsl::SourceRange & r, std::string_view identifier) {
      push_loc(std::string(uri), doc->text, r, identifier);
    };

    // Node / decorator / subtree definition: use the current word.
    if (auto w = word_at(doc->text, byte_offset)) {
      if (auto found = find_node_declaration_location(*doc, imported_uris, *w)) {
        push_loc(found->uri, found->doc->text, found->range, *w);
        return out;
      }
    }

    // Blackboard ref definition
    if (hit.tree && hit.inline_decl) {
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(hit.inline_decl->name, scope)) {
        push_loc_same_doc(sym->definition_range, sym->name);
        return out;
      }
    }

    // VarRef definition
    if (hit.tree && hit.var_ref) {
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(hit.var_ref->name, scope)) {
        push_loc_same_doc(sym->definition_range, sym->name);
        return out;
      }
    }

    // Fallback: resolve current word as symbol
    if (auto w = word_at(doc->text, byte_offset)) {
      const bt_dsl::Scope * scope = nullptr;
      if (hit.tree) {
        scope = analysis.symbols.tree_scope(hit.tree->name);
      }
      if (!scope) {
        scope = analysis.symbols.global_scope();
      }
      if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(*w, scope)) {
        // Best-effort: definition range is a byte range. Most symbols are
        // defined in the current document; imported globals are handled
        // separately via node/tree resolution above.
        push_loc_same_doc(sym->definition_range, sym->name);
        return out;
      }
    }

    return out;
  }

  json document_symbols_json_impl(std::string_view uri)
  {
    json out;
    out["uri"] = std::string(uri);
    out["symbols"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }

    ensure_parsed(*doc);

    auto push_sym = [&](std::string name, std::string kind, const bt_dsl::SourceRange & range) {
      json s;
      s["name"] = std::move(name);
      s["kind"] = std::move(kind);
      s["range"] = range_to_json(range);
      s["selectionRange"] = range_to_json(range);
      out["symbols"].push_back(std::move(s));
    };

    for (const auto & d : doc->program.declarations) {
      push_sym(d.name, "Declare", d.range);
    }

    for (const auto & g : doc->program.global_vars) {
      push_sym(g.name, "GlobalVar", g.range);
    }

    for (const auto & t : doc->program.trees) {
      push_sym(t.name, "Tree", t.range);
    }

    return out;
  }

  static std::string token_type_for_node_category(bt_dsl::NodeCategory c)
  {
    switch (c) {
      case bt_dsl::NodeCategory::Control:
        // Make control flow visually obvious.
        return "keyword";
      case bt_dsl::NodeCategory::SubTree:
        // Distinguish subtree calls from regular actions.
        return "class";
      case bt_dsl::NodeCategory::Decorator:
        return "decorator";
      case bt_dsl::NodeCategory::Action:
      case bt_dsl::NodeCategory::Condition:
        return "function";
    }
    return "function";
  }

  static std::string highlight_kind_from_direction(const std::optional<bt_dsl::PortDirection> & dir)
  {
    if (!dir) {
      return "Read";
    }
    switch (*dir) {
      case bt_dsl::PortDirection::In:
        return "Read";
      case bt_dsl::PortDirection::Out:
        return "Write";
      case bt_dsl::PortDirection::Ref:
        return "Read";
      case bt_dsl::PortDirection::Mut:
        return "Write";
    }
    return "Read";
  }

  json document_highlights_json_impl(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["items"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }
    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    const auto analysis = analyze_with_imports(uri, imported_uris);
    const auto hit = find_ast_hit(doc->program, byte_offset);
    if (!hit.tree) {
      return out;
    }

    auto push_item = [&](const bt_dsl::SourceRange & r, std::string kind) {
      json item;
      item["range"] = range_to_json(r);
      item["kind"] = std::move(kind);
      out["items"].push_back(std::move(item));
    };

    auto push_item_narrowed =
      [&](const bt_dsl::SourceRange & r, std::string_view ident, std::string kind) {
        push_item(narrow_to_identifier(doc->text, r, ident), std::move(kind));
      };

    // Collect all node name occurrences for a specific node call.
    auto collect_node_calls = [&](std::string_view node_name) {
      if (!hit.tree) {
        return;
      }

      std::function<void(const bt_dsl::Statement &)> visit_stmt;
      std::function<void(const bt_dsl::NodeStmt &)> visit_node;

      visit_node = [&](const bt_dsl::NodeStmt & n) {
        if (n.node_name == node_name) {
          push_item_narrowed(n.range, node_name, "Text");
        }
        for (const auto & child : n.children) {
          visit_stmt(child);
        }
      };

      visit_stmt = [&](const bt_dsl::Statement & s) {
        if (const auto * n = std::get_if<bt_dsl::Box<bt_dsl::NodeStmt>>(&s)) {
          visit_node(**n);
        }
      };

      for (const auto & stmt : hit.tree->body) {
        visit_stmt(stmt);
      }

      // Also highlight same-document declaration name, if present.
      for (const auto & d : doc->program.declarations) {
        if (d.name == node_name) {
          push_item_narrowed(d.range, node_name, "Text");
        }
      }
      for (const auto & t : doc->program.trees) {
        if (t.name == node_name) {
          push_item_narrowed(t.range, node_name, "Text");
        }
      }
    };

    // Collect all symbol occurrences (bb/var refs + assignments) within a
    // tree, comparing resolution results.
    auto collect_symbol_occurrences = [&](const bt_dsl::Symbol * target_sym) {
      if (!target_sym || !hit.tree) {
        return;
      }
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      if (!scope) {
        return;
      }

      auto maybe_push_definition = [&]() {
        // Best-effort: only include if in bounds for this document.
        if (target_sym->definition_range.end_byte <= doc->text.size()) {
          push_item_narrowed(target_sym->definition_range, target_sym->name, "Write");
        }
      };

      std::function<void(const bt_dsl::Expression &, const std::optional<bt_dsl::PortDirection> &)>
        visit_expr;
      std::function<void(
        const bt_dsl::ArgumentValue &, const std::optional<bt_dsl::PortDirection> &)>
        visit_arg_value;
      std::function<void(const bt_dsl::Statement &)> visit_stmt;
      std::function<void(const bt_dsl::NodeStmt &)> visit_node;

      visit_expr =
        [&](const bt_dsl::Expression & e, const std::optional<bt_dsl::PortDirection> & dir) {
          std::visit(
            [&](const auto & node) {
              using T = std::decay_t<decltype(node)>;
              if constexpr (std::is_same_v<T, bt_dsl::Literal>) {
                (void)node;
              } else if constexpr (std::is_same_v<T, bt_dsl::VarRef>) {
                if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(node.name, scope)) {
                  if (sym == target_sym) {
                    push_item_narrowed(
                      node.range, node.name,
                      highlight_kind_from_direction(dir ? dir : node.direction));
                  }
                }
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::BinaryExpr>>) {
                visit_expr(node->left, dir);
                visit_expr(node->right, dir);
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::UnaryExpr>>) {
                visit_expr(node->operand, dir);
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::CastExpr>>) {
                visit_expr(node->expr, dir);
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::IndexExpr>>) {
                visit_expr(node->base, dir);
                visit_expr(node->index, dir);
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::ArrayLiteralExpr>>) {
                for (const auto & el : node->elements) {
                  visit_expr(el, dir);
                }
                if (node->repeat_value) {
                  visit_expr(*node->repeat_value, dir);
                }
                if (node->repeat_count) {
                  visit_expr(*node->repeat_count, dir);
                }
              } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::VecMacroExpr>>) {
                for (const auto & el : node->value.elements) {
                  visit_expr(el, dir);
                }
                if (node->value.repeat_value) {
                  visit_expr(*node->value.repeat_value, dir);
                }
                if (node->value.repeat_count) {
                  visit_expr(*node->value.repeat_count, dir);
                }
              }
            },
            e);
        };

      visit_arg_value =
        [&](const bt_dsl::ArgumentValue & v, const std::optional<bt_dsl::PortDirection> & dir) {
          std::visit(
            [&](const auto & av) {
              using T = std::decay_t<decltype(av)>;
              if constexpr (std::is_same_v<T, bt_dsl::Expression>) {
                visit_expr(av, dir);
              } else if constexpr (std::is_same_v<T, bt_dsl::InlineBlackboardDecl>) {
                if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(av.name, scope)) {
                  if (sym == target_sym) {
                    push_item_narrowed(
                      av.range, av.name, highlight_kind_from_direction(bt_dsl::PortDirection::Out));
                  }
                }
              }
            },
            v);
        };

      visit_node = [&](const bt_dsl::NodeStmt & n) {
        for (const auto & pc : n.preconditions) {
          visit_expr(pc.condition, std::nullopt);
        }

        for (const auto & arg : n.args) {
          visit_arg_value(arg.value, arg.direction);
        }

        for (const auto & child : n.children) {
          visit_stmt(child);
        }
      };

      visit_stmt = [&](const bt_dsl::Statement & s) {
        std::visit(
          [&](const auto & elem) {
            using T = std::decay_t<decltype(elem)>;
            if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::NodeStmt>>) {
              visit_node(*elem);
            } else if constexpr (std::is_same_v<T, bt_dsl::AssignmentStmt>) {
              // Highlight assignment target if it resolves to the symbol.
              if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(elem.target, scope)) {
                if (sym == target_sym) {
                  push_item_narrowed(elem.range, elem.target, "Write");
                }
              }
              for (const auto & idx : elem.indices) {
                visit_expr(idx, std::nullopt);
              }
              visit_expr(elem.value, std::nullopt);
            } else if constexpr (std::is_same_v<T, bt_dsl::BlackboardDeclStmt>) {
              if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(elem.name, scope)) {
                if (sym == target_sym) {
                  push_item_narrowed(elem.range, elem.name, "Write");
                }
              }
              if (elem.initial_value) {
                visit_expr(*elem.initial_value, std::nullopt);
              }
            } else if constexpr (std::is_same_v<T, bt_dsl::ConstDeclStmt>) {
              if (const bt_dsl::Symbol * sym = analysis.symbols.resolve(elem.name, scope)) {
                if (sym == target_sym) {
                  push_item_narrowed(elem.range, elem.name, "Write");
                }
              }
              visit_expr(elem.value, std::nullopt);
            }
          },
          s);
      };

      for (const auto & stmt : hit.tree->body) {
        visit_stmt(stmt);
      }

      maybe_push_definition();
    };

    // Node call highlight
    if (hit.node && is_on_node_name(doc->text, *hit.node, byte_offset)) {
      collect_node_calls(hit.node->node_name);
      return out;
    }

    // Inline out-var decl highlight
    if (hit.inline_decl) {
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      const bt_dsl::Symbol * sym =
        scope ? analysis.symbols.resolve(hit.inline_decl->name, scope) : nullptr;
      collect_symbol_occurrences(sym);
      return out;
    }

    // Var ref highlight
    if (hit.var_ref) {
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      const bt_dsl::Symbol * sym =
        scope ? analysis.symbols.resolve(hit.var_ref->name, scope) : nullptr;
      collect_symbol_occurrences(sym);
      return out;
    }

    // Fallback: resolve current word as symbol
    if (auto w = word_at(doc->text, byte_offset)) {
      const bt_dsl::Scope * scope = analysis.symbols.tree_scope(hit.tree->name);
      const bt_dsl::Symbol * sym = scope ? analysis.symbols.resolve(*w, scope) : nullptr;
      collect_symbol_occurrences(sym);
    }

    return out;
  }

  json semantic_tokens_json_impl(
    std::string_view uri, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["tokens"] = json::array();

    auto * doc = get_doc(uri);
    if (!doc) {
      return out;
    }
    ensure_parsed(*doc);

    const auto analysis = analyze_with_imports(uri, imported_uris);

    auto push_token =
      [&](const bt_dsl::SourceRange & r, std::string type, std::string_view modifier = {}) {
        if (r.end_byte <= r.start_byte) {
          return;
        }
        json t;
        t["range"] = range_to_json(r);
        t["type"] = std::move(type);
        if (!modifier.empty()) {
          json mods = json::array();
          mods.push_back(std::string(modifier));
          t["modifiers"] = std::move(mods);
        }
        out["tokens"].push_back(std::move(t));
      };

    // Declarations
    for (const auto & d : doc->program.declarations) {
      // Category keyword
      push_token(narrow_to_identifier(doc->text, d.range, d.category), "keyword");

      auto cat = bt_dsl::node_category_from_string(d.category);
      const std::string name_type = cat ? token_type_for_node_category(*cat) : "function";
      push_token(narrow_to_identifier(doc->text, d.range, d.name), name_type, "declaration");

      for (const auto & p : d.ports) {
        push_token(narrow_to_identifier(doc->text, p.range, p.name), "parameter", "declaration");
        if (!p.type_name.empty()) {
          push_token(narrow_to_identifier(doc->text, p.range, p.type_name), "type");
        }
      }
    }

    // Global variables
    for (const auto & g : doc->program.global_vars) {
      push_token(narrow_to_identifier(doc->text, g.range, g.name), "variable", "declaration");
      if (g.type_name) {
        push_token(narrow_to_identifier(doc->text, g.range, *g.type_name), "type");
      }
    }

    // Trees and bodies
    std::function<void(const bt_dsl::Expression &, const std::optional<bt_dsl::PortDirection> &)>
      visit_expr;
    std::function<void(const bt_dsl::ArgumentValue &, const std::optional<bt_dsl::PortDirection> &)>
      visit_arg_value;
    std::function<void(const bt_dsl::Statement &)> visit_stmt;
    std::function<void(const bt_dsl::NodeStmt &)> visit_node;

    auto is_write_dir = [](const std::optional<bt_dsl::PortDirection> & dir) {
      if (!dir) {
        return false;
      }
      return *dir == bt_dsl::PortDirection::Out || *dir == bt_dsl::PortDirection::Mut;
    };

    visit_expr = [&](
                   const bt_dsl::Expression & e, const std::optional<bt_dsl::PortDirection> & dir) {
      std::visit(
        [&](const auto & node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, bt_dsl::Literal>) {
            (void)node;
          } else if constexpr (std::is_same_v<T, bt_dsl::VarRef>) {
            const std::string_view mod =
              is_write_dir(dir ? dir : node.direction) ? "modification" : std::string_view{};
            push_token(narrow_to_identifier(doc->text, node.range, node.name), "variable", mod);
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::BinaryExpr>>) {
            visit_expr(node->left, dir);
            visit_expr(node->right, dir);
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::UnaryExpr>>) {
            visit_expr(node->operand, dir);
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::CastExpr>>) {
            visit_expr(node->expr, dir);
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::IndexExpr>>) {
            visit_expr(node->base, dir);
            visit_expr(node->index, dir);
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::ArrayLiteralExpr>>) {
            for (const auto & el : node->elements) {
              visit_expr(el, dir);
            }
            if (node->repeat_value) {
              visit_expr(*node->repeat_value, dir);
            }
            if (node->repeat_count) {
              visit_expr(*node->repeat_count, dir);
            }
          } else if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::VecMacroExpr>>) {
            for (const auto & el : node->value.elements) {
              visit_expr(el, dir);
            }
            if (node->value.repeat_value) {
              visit_expr(*node->value.repeat_value, dir);
            }
            if (node->value.repeat_count) {
              visit_expr(*node->value.repeat_count, dir);
            }
          }
        },
        e);
    };

    visit_arg_value =
      [&](const bt_dsl::ArgumentValue & v, const std::optional<bt_dsl::PortDirection> & dir) {
        std::visit(
          [&](const auto & av) {
            using T = std::decay_t<decltype(av)>;
            if constexpr (std::is_same_v<T, bt_dsl::Expression>) {
              visit_expr(av, dir);
            } else if constexpr (std::is_same_v<T, bt_dsl::InlineBlackboardDecl>) {
              push_token(
                narrow_to_identifier(doc->text, av.range, av.name), "variable", "declaration");
            }
          },
          v);
      };

    visit_node = [&](const bt_dsl::NodeStmt & n) {
      // Node name (Action/SubTree/Control/...)
      std::string node_type = "function";
      if (const bt_dsl::NodeInfo * info = analysis.nodes.get_node(n.node_name)) {
        node_type = token_type_for_node_category(info->category);
      }
      push_token(narrow_to_identifier(doc->text, n.range, n.node_name), node_type);

      for (const auto & pc : n.preconditions) {
        visit_expr(pc.condition, std::nullopt);
      }

      // Arguments
      for (const auto & arg : n.args) {
        if (arg.name) {
          push_token(narrow_to_identifier(doc->text, arg.range, *arg.name), "property");
        }
        visit_arg_value(arg.value, arg.direction);
      }

      // Children and assignments
      for (const auto & child : n.children) {
        visit_stmt(child);
      }
    };

    visit_stmt = [&](const bt_dsl::Statement & s) {
      std::visit(
        [&](const auto & elem) {
          using T = std::decay_t<decltype(elem)>;
          if constexpr (std::is_same_v<T, bt_dsl::Box<bt_dsl::NodeStmt>>) {
            visit_node(*elem);
          } else if constexpr (std::is_same_v<T, bt_dsl::AssignmentStmt>) {
            push_token(
              narrow_to_identifier(doc->text, elem.range, elem.target), "variable", "modification");
            for (const auto & idx : elem.indices) {
              visit_expr(idx, std::nullopt);
            }
            visit_expr(elem.value, std::nullopt);
          } else if constexpr (std::is_same_v<T, bt_dsl::BlackboardDeclStmt>) {
            push_token(
              narrow_to_identifier(doc->text, elem.range, elem.name), "variable", "declaration");
            if (elem.type_name) {
              push_token(narrow_to_identifier(doc->text, elem.range, *elem.type_name), "type");
            }
            if (elem.initial_value) {
              visit_expr(*elem.initial_value, std::nullopt);
            }
          } else if constexpr (std::is_same_v<T, bt_dsl::ConstDeclStmt>) {
            push_token(
              narrow_to_identifier(doc->text, elem.range, elem.name), "variable", "declaration");
            if (elem.type_name) {
              push_token(narrow_to_identifier(doc->text, elem.range, *elem.type_name), "type");
            }
            visit_expr(elem.value, std::nullopt);
          }
        },
        s);
    };

    for (const auto & t : doc->program.trees) {
      push_token(narrow_to_identifier(doc->text, t.range, t.name), "function", "declaration");

      for (const auto & p : t.params) {
        push_token(narrow_to_identifier(doc->text, p.range, p.name), "parameter", "declaration");
        push_token(narrow_to_identifier(doc->text, p.range, p.type_name), "type");
        if (p.default_value) {
          visit_expr(*p.default_value, std::nullopt);
        }
      }

      for (const auto & stmt : t.body) {
        visit_stmt(stmt);
      }
    }

    return out;
  }
};

// =============================================================================
// Workspace public API
// =============================================================================

Workspace::Workspace() : impl_(new Impl()) {}
Workspace::~Workspace() { delete impl_; }

Workspace::Workspace(Workspace && other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
Workspace & Workspace::operator=(Workspace && other) noexcept
{
  if (this == &other) {
    return *this;
  }
  delete impl_;
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

void Workspace::set_document(std::string uri, std::string text)
{
  if (!impl_) {
    return;
  }
  Impl::Document d;
  d.uri = uri;
  d.text = std::move(text);
  d.parsed = false;
  impl_->docs[std::move(uri)] = std::move(d);
}

void Workspace::remove_document(std::string_view uri)
{
  if (!impl_) {
    return;
  }
  impl_->docs.erase(std::string(uri));
}

bool Workspace::has_document(std::string_view uri) const
{
  if (!impl_) {
    return false;
  }
  return impl_->docs.find(std::string(uri)) != impl_->docs.end();
}

std::string Workspace::diagnostics_json(std::string_view uri) { return diagnostics_json(uri, {}); }

std::string Workspace::diagnostics_json(
  std::string_view uri, const std::vector<std::string> & imported_uris)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->diagnostics_json_impl(uri, imported_uris).dump();
}

std::string Workspace::completion_json(std::string_view uri, uint32_t byte_offset)
{
  return completion_json(uri, byte_offset, {}, {});
}

std::string Workspace::completion_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris,
  std::string_view trigger)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->completion_json_impl(uri, byte_offset, imported_uris, trigger).dump();
}

std::string Workspace::hover_json(std::string_view uri, uint32_t byte_offset)
{
  return hover_json(uri, byte_offset, {});
}

std::string Workspace::hover_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->hover_json_impl(uri, byte_offset, imported_uris).dump();
}

std::string Workspace::definition_json(std::string_view uri, uint32_t byte_offset)
{
  return definition_json(uri, byte_offset, {});
}

std::string Workspace::definition_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->definition_json_impl(uri, byte_offset, imported_uris).dump();
}

std::string Workspace::document_symbols_json(std::string_view uri)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->document_symbols_json_impl(uri).dump();
}

std::string Workspace::document_highlights_json(std::string_view uri, uint32_t byte_offset)
{
  return document_highlights_json(uri, byte_offset, {});
}

std::string Workspace::document_highlights_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->document_highlights_json_impl(uri, byte_offset, imported_uris).dump();
}

std::string Workspace::semantic_tokens_json(std::string_view uri)
{
  return semantic_tokens_json(uri, {});
}

std::string Workspace::semantic_tokens_json(
  std::string_view uri, const std::vector<std::string> & imported_uris)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->semantic_tokens_json_impl(uri, imported_uris).dump();
}

std::string Workspace::resolve_imports_json(std::string_view uri, std::string_view stdlib_uri)
{
  if (!impl_) {
    return "{}";
  }
  return impl_->resolve_imports_json_impl(uri, stdlib_uri).dump();
}

}  // namespace bt_dsl::lsp
