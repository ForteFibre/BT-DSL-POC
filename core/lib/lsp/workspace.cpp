#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/lsp/completion_context.hpp"
#include "bt_dsl/lsp/lsp.hpp"
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/null_checker.hpp"
#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/sema/types/type_utils.hpp"
#include "bt_dsl/syntax/frontend.hpp"
#include "bt_dsl/syntax/keywords.hpp"

namespace bt_dsl::lsp
{
namespace
{

using json = nlohmann::json;
namespace fs = std::filesystem;

// -----------------------------
// Range helpers
// -----------------------------

bool contains_byte(const bt_dsl::SourceRange & r, uint32_t byte)
{
  return r.contains(bt_dsl::SourceLocation(r.file_id(), byte));
}

uint32_t clamp_byte_offset(uint32_t off, size_t text_size)
{
  if (off > text_size) {
    return static_cast<uint32_t>(text_size);
  }
  return off;
}

bool is_ident_char(unsigned char c) { return std::isalnum(c) != 0 || c == '_'; }

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

  if (
    byte_offset < text.size() && std::isspace(static_cast<unsigned char>(text[byte_offset])) != 0) {
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

json range_to_json(const bt_dsl::FullSourceRange & r)
{
  return json{
    {"startByte", r.start_byte},     {"endByte", r.end_byte}, {"startLine", r.start_line},
    {"startColumn", r.start_column}, {"endLine", r.end_line}, {"endColumn", r.end_column},
  };
}

json byte_range_to_json(const ByteRange & r)
{
  return json{{"startByte", r.startByte}, {"endByte", r.endByte}};
}

bt_dsl::SourceRange narrow_to_identifier(
  std::string_view text, bt_dsl::SourceRange decl_range, std::string_view ident)
{
  const uint32_t start =
    std::min<uint32_t>(decl_range.get_begin().get_offset(), static_cast<uint32_t>(text.size()));
  const uint32_t end =
    std::min<uint32_t>(decl_range.get_end().get_offset(), static_cast<uint32_t>(text.size()));

  if (end <= start) {
    return decl_range;
  }

  const std::string_view slice = text.substr(start, end - start);
  const size_t pos = slice.find(ident);
  if (pos == std::string_view::npos) {
    return decl_range;
  }

  const uint32_t sb = start + static_cast<uint32_t>(pos);
  const uint32_t eb = sb + static_cast<uint32_t>(ident.size());
  return {decl_range.file_id(), sb, eb};
}

// -----------------------------
// Import URI resolution (same policy as core)
// -----------------------------

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
  std::string out = "bt-dsl-pkg://";
  out.append(spec.data(), spec.size());
  return out;
}

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
  if (!is_relative_import_spec(spec)) {
    return std::nullopt;
  }

  constexpr std::string_view file_prefix = "file://";
  if (!starts_with(from_uri, file_prefix)) {
    return std::nullopt;
  }

  const size_t last_slash = from_uri.find_last_of('/');
  if (last_slash == std::string_view::npos || last_slash + 1 <= file_prefix.size()) {
    return std::nullopt;
  }

  const std::string_view dir_uri = from_uri.substr(0, last_slash + 1);
  std::string combined = std::string(dir_uri);
  combined += std::string(spec);

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

std::optional<fs::path> file_uri_to_path(std::string_view uri)
{
  if (!starts_with(uri, "file://")) {
    return std::nullopt;
  }

  const std::string_view rest = uri.substr(std::string_view("file://").size());
  if (!starts_with(rest, "/")) {
    return std::nullopt;
  }

  // Note: URI percent-decoding is intentionally omitted here. The LSP server
  // layer performs decode when reading from disk. The serverless workspace
  // only uses the path for SourceManager display.
  return fs::path(std::string(rest));
}

// -----------------------------
// Type stringification (minimal)
// -----------------------------

std::string type_to_string(const bt_dsl::Type * t)
{
  if (t == nullptr) {
    return "?";
  }

  using bt_dsl::TypeKind;
  switch (t->kind) {
    case TypeKind::Int8:
      return "int8";
    case TypeKind::Int16:
      return "int16";
    case TypeKind::Int32:
      return "int32";
    case TypeKind::Int64:
      return "int64";
    case TypeKind::UInt8:
      return "uint8";
    case TypeKind::UInt16:
      return "uint16";
    case TypeKind::UInt32:
      return "uint32";
    case TypeKind::UInt64:
      return "uint64";
    case TypeKind::Float32:
      return "float32";
    case TypeKind::Float64:
      return "float64";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::String:
      return "string";
    case TypeKind::BoundedString:
      return "string<" + std::to_string(t->size) + ">";
    case TypeKind::StaticArray:
      return "[" + type_to_string(t->element_type) + "; " + std::to_string(t->size) + "]";
    case TypeKind::BoundedArray:
      return "[" + type_to_string(t->element_type) + "; <=" + std::to_string(t->size) + "]";
    case TypeKind::DynamicArray:
      return "vec<" + type_to_string(t->element_type) + ">";
    case TypeKind::Nullable:
      return type_to_string(t->base_type) + "?";
    case TypeKind::Extern:
      return std::string(t->name);
    case TypeKind::IntegerLiteral:
      return "{integer}";
    case TypeKind::FloatLiteral:
      return "{float}";
    case TypeKind::NullLiteral:
      return "null";
    case TypeKind::Unknown:
      return "?";
    case TypeKind::Error:
      return "<error>";
  }
  return "?";
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

// -----------------------------
// AST hit testing (minimal subset)
// -----------------------------

struct AstHit
{
  bt_dsl::TreeDecl * tree = nullptr;
  bt_dsl::NodeStmt * node_stmt = nullptr;
  bt_dsl::InlineBlackboardDecl * inline_decl = nullptr;
  bt_dsl::VarRefExpr * var_ref = nullptr;
};

bt_dsl::TreeDecl * find_tree_at(bt_dsl::Program & p, uint32_t off)
{
  bt_dsl::TreeDecl * best = nullptr;
  uint32_t best_len = std::numeric_limits<uint32_t>::max();

  for (auto * t : p.trees()) {
    if (t == nullptr) continue;
    if (!contains_byte(t->get_range(), off)) {
      continue;
    }

    const auto s = t->get_range().get_begin().get_offset();
    const auto e = t->get_range().get_end().get_offset();
    const uint32_t len = (e > s) ? (e - s) : 0;

    if (best == nullptr || len < best_len) {
      best = t;
      best_len = len;
    }
  }

  return best;
}

void consider_best_varref(bt_dsl::VarRefExpr * vr, uint32_t off, AstHit & hit)
{
  if (vr == nullptr) return;
  if (!contains_byte(vr->get_range(), off)) return;

  const auto s = vr->get_range().get_begin().get_offset();
  const auto e = vr->get_range().get_end().get_offset();
  const uint32_t new_len = (e > s) ? (e - s) : 0;

  if (hit.var_ref == nullptr) {
    hit.var_ref = vr;
    return;
  }

  const auto cs = hit.var_ref->get_range().get_begin().get_offset();
  const auto ce = hit.var_ref->get_range().get_end().get_offset();
  const uint32_t cur_len = (ce > cs) ? (ce - cs) : 0;
  if (new_len < cur_len) {
    hit.var_ref = vr;
  }
}

void consider_best_inline_decl(bt_dsl::InlineBlackboardDecl * d, uint32_t off, AstHit & hit)
{
  if (d == nullptr) return;
  if (!contains_byte(d->get_range(), off)) return;

  const auto s = d->get_range().get_begin().get_offset();
  const auto e = d->get_range().get_end().get_offset();
  const uint32_t new_len = (e > s) ? (e - s) : 0;

  if (hit.inline_decl == nullptr) {
    hit.inline_decl = d;
    return;
  }

  const auto cs = hit.inline_decl->get_range().get_begin().get_offset();
  const auto ce = hit.inline_decl->get_range().get_end().get_offset();
  const uint32_t cur_len = (ce > cs) ? (ce - cs) : 0;
  if (new_len < cur_len) {
    hit.inline_decl = d;
  }
}

void consider_best_node_stmt(bt_dsl::NodeStmt * n, uint32_t off, AstHit & hit)
{
  if (n == nullptr) return;
  if (!contains_byte(n->get_range(), off)) return;

  const auto s = n->get_range().get_begin().get_offset();
  const auto e = n->get_range().get_end().get_offset();
  const uint32_t new_len = (e > s) ? (e - s) : 0;

  if (hit.node_stmt == nullptr) {
    hit.node_stmt = n;
    return;
  }

  const auto cs = hit.node_stmt->get_range().get_begin().get_offset();
  const auto ce = hit.node_stmt->get_range().get_end().get_offset();
  const uint32_t cur_len = (ce > cs) ? (ce - cs) : 0;
  if (new_len < cur_len) {
    hit.node_stmt = n;
  }
}

void visit_expr_for_hit(bt_dsl::Expr * e, uint32_t off, AstHit & hit);
void visit_stmt_for_hit(bt_dsl::Stmt * s, uint32_t off, AstHit & hit);

void visit_expr_for_hit(bt_dsl::Expr * e, uint32_t off, AstHit & hit)
{
  if (e == nullptr) return;
  if (!contains_byte(e->get_range(), off)) return;

  if (auto * vr = bt_dsl::dyn_cast<bt_dsl::VarRefExpr>(e)) {
    consider_best_varref(vr, off, hit);
    return;
  }

  if (auto * b = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(e)) {
    visit_expr_for_hit(b->lhs, off, hit);
    visit_expr_for_hit(b->rhs, off, hit);
    return;
  }

  if (auto * u = bt_dsl::dyn_cast<bt_dsl::UnaryExpr>(e)) {
    visit_expr_for_hit(u->operand, off, hit);
    return;
  }

  if (auto * c = bt_dsl::dyn_cast<bt_dsl::CastExpr>(e)) {
    visit_expr_for_hit(c->expr, off, hit);
    return;
  }

  if (auto * idx = bt_dsl::dyn_cast<bt_dsl::IndexExpr>(e)) {
    visit_expr_for_hit(idx->base, off, hit);
    visit_expr_for_hit(idx->index, off, hit);
    return;
  }

  if (auto * arr = bt_dsl::dyn_cast<bt_dsl::ArrayLiteralExpr>(e)) {
    for (auto * el : arr->elements) {
      visit_expr_for_hit(el, off, hit);
    }
    return;
  }

  if (auto * rep = bt_dsl::dyn_cast<bt_dsl::ArrayRepeatExpr>(e)) {
    visit_expr_for_hit(rep->value, off, hit);
    visit_expr_for_hit(rep->count, off, hit);
    return;
  }

  if (auto * vm = bt_dsl::dyn_cast<bt_dsl::VecMacroExpr>(e)) {
    // VecMacroExpr wraps either an ArrayLiteralExpr or ArrayRepeatExpr.
    visit_expr_for_hit(vm->inner, off, hit);
    return;
  }
}

void visit_stmt_for_hit(bt_dsl::Stmt * s, uint32_t off, AstHit & hit)
{
  if (s == nullptr) return;
  if (!contains_byte(s->get_range(), off)) return;

  if (auto * ns = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
    consider_best_node_stmt(ns, off, hit);
    for (auto * pc : ns->preconditions) {
      if (pc && contains_byte(pc->get_range(), off)) {
        visit_expr_for_hit(pc->condition, off, hit);
      }
    }
    for (auto * arg : ns->args) {
      if (arg == nullptr) continue;
      if (!contains_byte(arg->get_range(), off)) continue;
      if (arg->inlineDecl) {
        consider_best_inline_decl(arg->inlineDecl, off, hit);
      }
      if (arg->valueExpr) {
        visit_expr_for_hit(arg->valueExpr, off, hit);
      }
    }
    for (auto * child : ns->children) {
      visit_stmt_for_hit(child, off, hit);
    }
    return;
  }

  if (auto * as = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(s)) {
    for (auto * pc : as->preconditions) {
      if (pc && contains_byte(pc->get_range(), off)) {
        visit_expr_for_hit(pc->condition, off, hit);
      }
    }
    for (auto * idx : as->indices) {
      visit_expr_for_hit(idx, off, hit);
    }
    visit_expr_for_hit(as->value, off, hit);
    return;
  }

  if (auto * vd = bt_dsl::dyn_cast<bt_dsl::BlackboardDeclStmt>(s)) {
    if (vd->initialValue) {
      visit_expr_for_hit(vd->initialValue, off, hit);
    }
    return;
  }

  if (auto * cd = bt_dsl::dyn_cast<bt_dsl::ConstDeclStmt>(s)) {
    visit_expr_for_hit(cd->value, off, hit);
    return;
  }
}

AstHit find_ast_hit(bt_dsl::Program & p, uint32_t off)
{
  AstHit hit;
  hit.tree = find_tree_at(p, off);
  if (hit.tree == nullptr) {
    return hit;
  }

  for (auto * stmt : hit.tree->body) {
    visit_stmt_for_hit(stmt, off, hit);
  }

  return hit;
}

// -----------------------------
// Built-in nodes
// -----------------------------

std::vector<std::string> builtin_node_candidates()
{
  return {
    "Sequence", "Fallback", "Parallel", "ReactiveSequence", "ReactiveFallback",
  };
}

std::string format_port(std::string_view direction, std::string_view name, std::string_view type)
{
  std::string out;
  out += std::string(direction);
  out += " ";
  out += std::string(name);
  if (!type.empty()) {
    out += ": ";
    out += std::string(type);
  }
  return out;
}

struct PortSig
{
  std::string name;
  std::string direction;
  std::string type;
};

std::optional<ExternNodeCategory> extern_category_from_decl(const bt_dsl::AstNode * n)
{
  if (const auto * e = bt_dsl::dyn_cast<bt_dsl::ExternDecl>(n)) {
    return e->category;
  }
  return std::nullopt;
}

std::string token_type_for_node_category(std::optional<ExternNodeCategory> c, bool is_tree)
{
  if (is_tree) {
    return "class";
  }
  if (!c) {
    return "function";
  }

  switch (*c) {
    case ExternNodeCategory::Control:
      return "keyword";
    case ExternNodeCategory::Subtree:
      return "class";
    case ExternNodeCategory::Decorator:
      return "decorator";
    case ExternNodeCategory::Action:
    case ExternNodeCategory::Condition:
      return "function";
  }

  return "function";
}

}  // namespace

// =============================================================================
// Workspace::Impl
// =============================================================================

struct Workspace::Impl
{
  struct Document
  {
    std::string uri;
    std::string text;

    bt_dsl::ModuleInfo module{};

    std::unique_ptr<bt_dsl::TypeContext> type_ctx;

    bool indexed = false;
    bool analyzed = false;
    uint64_t analyzed_import_hash = 0;

    bt_dsl::DiagnosticBag sema_diags;
  };

  bt_dsl::SourceRegistry sources;

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
    if (d.module.program != nullptr && d.module.ast) {
      return;
    }

    // Re-parse into a fresh AST context.
    d.module.ast = std::make_unique<bt_dsl::AstContext>();
    d.module.parse_diags = bt_dsl::DiagnosticBag{};

    const fs::path path = file_uri_to_path(d.uri).value_or(fs::path{d.uri});
    const bt_dsl::ParseOutput out =
      bt_dsl::parse_source(sources, path, d.text, *d.module.ast, d.module.parse_diags);
    d.module.file_id = out.file_id;
    d.module.program = out.program;
  }

  void ensure_indexed(Document & d)
  {
    ensure_parsed(d);
    if (d.indexed) {
      return;
    }

    d.module.types = bt_dsl::TypeTable{};
    d.module.nodes = bt_dsl::NodeRegistry{};
    d.module.values = bt_dsl::SymbolTable{};
    d.module.imports.clear();

    d.module.types.register_builtins();

    for (const auto * ext_type : d.module.program->extern_types()) {
      bt_dsl::TypeSymbol sym;
      sym.name = ext_type->name;
      sym.decl = ext_type;
      sym.is_builtin = false;
      d.module.types.define(sym);
    }

    for (const auto * alias : d.module.program->type_aliases()) {
      bt_dsl::TypeSymbol sym;
      sym.name = alias->name;
      sym.decl = alias;
      sym.is_builtin = false;
      d.module.types.define(sym);
    }

    for (const auto * ext : d.module.program->externs()) {
      bt_dsl::NodeSymbol sym;
      sym.name = ext->name;
      sym.decl = ext;
      d.module.nodes.define(sym);
    }
    for (const auto * tree : d.module.program->trees()) {
      bt_dsl::NodeSymbol sym;
      sym.name = tree->name;
      sym.decl = tree;
      d.module.nodes.define(sym);
    }

    bt_dsl::SymbolTableBuilder stb(d.module.values, d.module.types, d.module.nodes, nullptr);
    (void)stb.build(*d.module.program);

    d.indexed = true;
  }

  static uint64_t hash_imports(const std::vector<std::string> & imported_uris)
  {
    uint64_t h = 1469598103934665603ULL;
    for (const auto & s : imported_uris) {
      for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
      }
      h ^= 0xFF;
      h *= 1099511628211ULL;
    }
    return h;
  }

  void ensure_analyzed(Document & d, const std::vector<std::string> & imported_uris)
  {
    ensure_indexed(d);

    if (!d.type_ctx) {
      d.type_ctx = std::make_unique<bt_dsl::TypeContext>();
    }

    const uint64_t h = hash_imports(imported_uris);
    if (d.analyzed && d.analyzed_import_hash == h) {
      return;
    }

    // Ensure imported modules are indexed.
    std::vector<bt_dsl::ModuleInfo *> imports;
    imports.reserve(imported_uris.size());

    for (const auto & u : imported_uris) {
      auto * imp = get_doc(u);
      if (imp == nullptr) {
        continue;
      }
      ensure_indexed(*imp);
      imports.push_back(&imp->module);
    }

    d.module.imports = imports;

    bt_dsl::DiagnosticBag diags;

    bt_dsl::NameResolver resolver(d.module, &diags);
    (void)resolver.resolve();

    bt_dsl::TypeChecker tc(*d.type_ctx, d.module.types, d.module.values, &diags);
    (void)tc.check(*d.module.program);

    bt_dsl::InitializationChecker init_checker(d.module.values, d.module.nodes, &diags);
    (void)init_checker.check(*d.module.program);

    bt_dsl::NullChecker null_checker(d.module.values, d.module.nodes, &diags);
    (void)null_checker.check(*d.module.program);

    bt_dsl::TreeRecursionChecker recursion_checker(&diags);
    (void)recursion_checker.check(*d.module.program);

    d.sema_diags = std::move(diags);
    d.analyzed = true;
    d.analyzed_import_hash = h;
  }

  std::vector<std::string> direct_import_uris(Document & d, std::string_view stdlib_uri)
  {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    auto enqueue = [&](const std::string & u) {
      if (u.empty()) {
        return;
      }
      if (u == d.uri) {
        return;
      }
      if (!seen.insert(u).second) {
        return;
      }
      out.push_back(u);
    };

    if (!stdlib_uri.empty()) {
      enqueue(std::string(stdlib_uri));
    }

    ensure_parsed(d);

    bt_dsl::Program * p = d.module.program;
    if (p == nullptr) {
      return out;
    }

    for (auto * imp : p->imports()) {
      if (imp == nullptr) continue;
      const std::string_view spec = imp->path;

      if (is_relative_import_spec(spec)) {
        if (auto resolved = resolve_relative_import_uri(d.uri, spec)) {
          enqueue(*resolved);
        }
      } else {
        enqueue(package_import_uri(spec));
      }
    }

    return out;
  }

  json diagnostics_json_impl(std::string_view uri, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["items"] = json::array();

    auto * doc = get_doc(uri);
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);

    // Import diagnostics: policy + missing-doc checks.
    bt_dsl::Program * p = doc->module.program;
    if (p != nullptr) {
      for (auto * imp : p->imports()) {
        if (imp == nullptr) continue;
        const std::string_view spec = imp->path;

        auto push_item = [&](std::string msg) {
          json item;
          item["source"] = "import";
          item["message"] = std::move(msg);
          item["severity"] = "Error";
          const auto fr = sources.get_full_range(imp->get_range());
          item["range"] = range_to_json(fr);
          out["items"].push_back(std::move(item));
        };

        if (starts_with(spec, "/")) {
          push_item("Absolute import paths are not allowed: \"" + std::string(spec) + "\"");
          continue;
        }
        if (!has_required_extension(spec)) {
          push_item("Import path must include an extension: \"" + std::string(spec) + "\"");
          continue;
        }

        if (is_relative_import_spec(spec)) {
          const auto resolved = resolve_relative_import_uri(doc->uri, spec);
          if (!resolved) {
            push_item("Cannot resolve relative import against this document URI");
            continue;
          }
          if (docs.find(*resolved) == docs.end()) {
            push_item("Imported document is not loaded: \"" + std::string(spec) + "\"");
          }
          continue;
        }

        // For package imports, check if any of the imported_uris (resolved by host)
        // correspond to this import. The host resolves bt-dsl-pkg:// to file:// URIs.
        // If the host provides imported_uris, trust that package imports are resolved.
        // Only error if no imported_uris are provided at all (legacy behavior).
        const std::string pkg_uri = package_import_uri(spec);
        bool found_in_imports = false;
        for (const auto & imp_uri : imported_uris) {
          // The imported_uris contains file:// URIs resolved by host
          if (get_doc(imp_uri) != nullptr) {
            // Check if this import spec is likely handled by this URI
            // by checking if the URI ends with the import path
            if (
              imp_uri.size() >= spec.size() &&
              imp_uri.substr(imp_uri.size() - spec.size()) == spec) {
              found_in_imports = true;
              break;
            }
          }
        }
        // Also check if the pkg_uri itself is loaded (for backwards compatibility)
        if (!found_in_imports && docs.find(pkg_uri) == docs.end()) {
          // Check if any doc path ends with the import spec
          for (const auto & [doc_uri, _] : docs) {
            if (
              doc_uri.size() >= spec.size() &&
              doc_uri.substr(doc_uri.size() - spec.size()) == spec) {
              found_in_imports = true;
              break;
            }
          }
        }
        if (!found_in_imports) {
          push_item(
            "Cannot resolve package import (host must provide it): \"" + std::string(spec) + "\"");
        }
      }
    }

    // Parse/build diagnostics
    for (const auto & d0 : doc->module.parse_diags.all()) {
      json item;
      item["source"] = "parser";
      item["message"] = d0.message;
      item["severity"] = severity_to_string(d0.severity);
      if (!d0.code.empty()) {
        item["code"] = d0.code;
      }
      const auto fr = sources.get_full_range(d0.primary_range());
      item["range"] = range_to_json(fr);
      out["items"].push_back(std::move(item));
    }

    const bool has_parse_error = std::any_of(
      doc->module.parse_diags.all().begin(), doc->module.parse_diags.all().end(),
      [](const bt_dsl::Diagnostic & d0) { return d0.severity == bt_dsl::Severity::Error; });

    if (!has_parse_error) {
      ensure_analyzed(*doc, imported_uris);
      for (const auto & d0 : doc->sema_diags.all()) {
        json item;
        item["source"] = "analyzer";
        item["message"] = d0.message;
        item["severity"] = severity_to_string(d0.severity);
        if (!d0.code.empty()) {
          item["code"] = d0.code;
        }
        const auto fr = sources.get_full_range(d0.primary_range());
        item["range"] = range_to_json(fr);
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

    auto * doc = get_doc(uri);
    if (doc == nullptr) {
      return out;
    }

    const auto uris = direct_import_uris(*doc, stdlib_uri);
    for (const auto & u : uris) {
      out["uris"].push_back(u);
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
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    ensure_analyzed(*doc, imported_uris);

    const auto replace_range = completion_replace_range_at(doc->text, byte_offset);

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
      for (const auto & ddir : bt_dsl::syntax::k_port_directions) {
        push_item(std::string(ddir), "Keyword", "direction", std::string(ddir) + " ");
      }
    };

    auto push_visible_vars = [&]() {
      const bt_dsl::Scope * scope = nullptr;
      if (ctx.tree_name) {
        scope = doc->module.values.get_tree_scope(*ctx.tree_name);
      }
      if (scope == nullptr) {
        scope = doc->module.values.get_global_scope();
      }

      std::unordered_set<std::string_view> seen;
      for (const bt_dsl::Scope * s = scope; s != nullptr; s = s->get_parent()) {
        for (const auto & [name, sym] : s->get_symbols()) {
          if (!seen.insert(name).second) {
            continue;
          }
          std::string detail;
          if (sym.typeName) {
            detail = std::string(*sym.typeName);
          }
          push_item(std::string(sym.name), "Variable", detail, std::string(sym.name));
        }
      }
    };

    auto collect_callable_ports = [&]() -> std::vector<PortSig> {
      std::vector<PortSig> ports;
      if (!ctx.callable_name) {
        return ports;
      }

      const bt_dsl::NodeSymbol * sym = doc->module.nodes.lookup(*ctx.callable_name);
      if (sym == nullptr) {
        // Prefer URIs explicitly provided by the host; this is more robust than
        // relying on d.module.imports being populated/cached.
        for (const auto & imp_uri : imported_uris) {
          auto * imp_doc = get_doc(imp_uri);
          if (imp_doc == nullptr) {
            continue;
          }
          ensure_indexed(*imp_doc);
          const bt_dsl::NodeSymbol * imported = imp_doc->module.nodes.lookup(*ctx.callable_name);
          if (imported != nullptr && bt_dsl::ModuleInfo::is_public(imported->name)) {
            sym = imported;
            break;
          }
        }
      }

      if (sym == nullptr || sym->decl == nullptr) {
        return ports;
      }

      if (const auto * ext = bt_dsl::dyn_cast<bt_dsl::ExternDecl>(sym->decl)) {
        for (auto * p : ext->ports) {
          if (p == nullptr) continue;
          PortSig ps;
          ps.name = std::string(p->name);
          ps.direction = p->direction ? std::string(bt_dsl::to_string(*p->direction)) : "";
          ps.type = "";
          if (p->type != nullptr) {
            // TypeExpr's printed name is not trivially available here; fallback to slice.
            const auto tr = p->type->get_range();
            const auto slice = sources.get_slice(tr);
            ps.type = std::string(slice);
          }
          ports.push_back(std::move(ps));
        }
        return ports;
      }

      if (const auto * tree = bt_dsl::dyn_cast<bt_dsl::TreeDecl>(sym->decl)) {
        for (auto * param : tree->params) {
          if (param == nullptr) continue;
          PortSig ps;
          ps.name = std::string(param->name);
          ps.direction = param->direction ? std::string(bt_dsl::to_string(*param->direction)) : "";
          ps.type = "";
          if (param->type != nullptr) {
            const auto tr = param->type->get_range();
            ps.type = std::string(sources.get_slice(tr));
          }
          ports.push_back(std::move(ps));
        }
        return ports;
      }

      return ports;
    };

    auto push_callable_ports = [&](bool include_colon) {
      for (const auto & p : collect_callable_ports()) {
        const std::string insert = include_colon ? (p.name + ": ") : p.name;
        const std::string dir = p.direction.empty() ? "" : p.direction;
        const std::string type = p.type.empty() ? "" : p.type;
        const std::string detail =
          (dir.empty() && type.empty()) ? "" : format_port(dir, p.name, type);
        push_item(p.name, "Port", detail, insert);
      }
    };

    if (ctx.kind == CompletionContextKind::ImportPath) {
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

    const bool want_nodes =
      (ctx.kind == CompletionContextKind::TreeBody || ctx.kind == CompletionContextKind::NodeName);
    if (!want_nodes) {
      return out;
    }

    std::vector<std::string> names;
    {
      // Local module externs + trees
      bt_dsl::Program * p0 = doc->module.program;
      if (p0) {
        for (auto * e : p0->externs()) {
          if (e) names.emplace_back(e->name);
        }
        for (auto * t : p0->trees()) {
          if (t) names.emplace_back(t->name);
        }
      }

      // Direct imports (public only)
      for (auto * imp : doc->module.imports) {
        if (imp == nullptr || imp->program == nullptr) continue;
        for (auto * e : imp->program->externs()) {
          if (e && bt_dsl::ModuleInfo::is_public(e->name)) {
            names.emplace_back(e->name);
          }
        }
        for (auto * t : imp->program->trees()) {
          if (t && bt_dsl::ModuleInfo::is_public(t->name)) {
            names.emplace_back(t->name);
          }
        }
      }

      auto builtins = builtin_node_candidates();
      names.insert(names.end(), builtins.begin(), builtins.end());
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    for (const auto & n : names) {
      std::string detail;
      if (const bt_dsl::NodeSymbol * sym = doc->module.nodes.lookup(n)) {
        if (auto c = extern_category_from_decl(sym->decl)) {
          detail = std::string(bt_dsl::to_string(*c));
        } else if (sym->is_tree()) {
          detail = "subtree";
        }
      }
      push_item(n, "Node", detail, n);
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
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());
    ensure_analyzed(*doc, imported_uris);

    auto hit = find_ast_hit(*doc->module.program, byte_offset);

    if (hit.tree != nullptr && (hit.var_ref != nullptr || hit.inline_decl != nullptr)) {
      const std::string name =
        hit.var_ref ? std::string(hit.var_ref->name) : std::string(hit.inline_decl->name);
      const bt_dsl::SourceRange r =
        hit.var_ref ? hit.var_ref->get_range() : hit.inline_decl->get_range();

      const bt_dsl::Scope * scope = doc->module.values.get_tree_scope(hit.tree->name);
      const bt_dsl::Symbol * sym = nullptr;
      if (hit.var_ref && hit.var_ref->resolvedSymbol) {
        sym = hit.var_ref->resolvedSymbol;
      } else if (scope) {
        sym = doc->module.values.resolve(name, scope);
      }

      std::string md;
      md += "**" + name + "**";

      std::optional<std::string> type_str;
      if (sym && sym->typeName) {
        type_str = std::string(*sym->typeName);
      } else if (hit.var_ref && hit.var_ref->resolvedType) {
        type_str = type_to_string(hit.var_ref->resolvedType);
      }

      if (type_str) {
        md += "\n\nType: `" + *type_str + "`";
      }

      out["contents"] = md;
      out["range"] = range_to_json(sources.get_full_range(r));
      return out;
    }

    if (auto w = word_at(doc->text, byte_offset)) {
      const bt_dsl::NodeSymbol * sym = doc->module.nodes.lookup(*w);
      if (sym == nullptr) {
        // Prefer URIs explicitly provided by the host.
        for (const auto & imp_uri : imported_uris) {
          auto * imp_doc = get_doc(imp_uri);
          if (imp_doc == nullptr) {
            continue;
          }
          ensure_indexed(*imp_doc);
          const bt_dsl::NodeSymbol * imported = imp_doc->module.nodes.lookup(*w);
          if (imported != nullptr && bt_dsl::ModuleInfo::is_public(imported->name)) {
            sym = imported;
            break;
          }
        }
      }

      if (sym != nullptr && sym->decl != nullptr) {
        std::string md;
        md += "**" + *w + "**";

        if (const auto * ext = bt_dsl::dyn_cast<bt_dsl::ExternDecl>(sym->decl)) {
          md += "\n\nCategory: `" + std::string(bt_dsl::to_string(ext->category)) + "`";
          if (!ext->ports.empty()) {
            md += "\n\nPorts:";
            for (auto * p : ext->ports) {
              if (p == nullptr) continue;
              const std::string dir =
                p->direction ? std::string(bt_dsl::to_string(*p->direction)) : "";
              const std::string ty =
                p->type ? std::string(sources.get_slice(p->type->get_range())) : "";
              md += "\n- `" + format_port(dir, p->name, ty) + "`";
            }
          }
        } else if (const auto * tree = bt_dsl::dyn_cast<bt_dsl::TreeDecl>(sym->decl)) {
          md += "\n\nCategory: `subtree`";
          if (!tree->params.empty()) {
            md += "\n\nPorts:";
            for (auto * p : tree->params) {
              if (p == nullptr) continue;
              const std::string dir =
                p->direction ? std::string(bt_dsl::to_string(*p->direction)) : "";
              const std::string ty =
                p->type ? std::string(sources.get_slice(p->type->get_range())) : "";
              md += "\n- `" + format_port(dir, p->name, ty) + "`";
            }
          }
        }

        out["contents"] = md;
        const auto wr = word_range_at(doc->text, byte_offset);
        out["range"] = range_to_json(sources.get_full_range(
          bt_dsl::SourceRange(doc->module.file_id, wr.startByte, wr.endByte)));
        return out;
      }

      // Value symbol (variable / const / param) hover fallback.
      const bt_dsl::TreeDecl * tree = hit.tree;
      if (tree == nullptr && doc->module.program != nullptr) {
        tree = find_tree_at(*doc->module.program, byte_offset);
      }

      const bt_dsl::Scope * scope = nullptr;
      if (tree != nullptr) {
        scope = doc->module.values.get_tree_scope(tree->name);
      }
      if (scope == nullptr) {
        scope = doc->module.values.get_global_scope();
      }

      if (const bt_dsl::Symbol * vsym = doc->module.values.resolve(*w, scope)) {
        std::string md;
        md += "**" + *w + "**";
        if (vsym->typeName) {
          md += "\n\nType: `" + std::string(*vsym->typeName) + "`";
        }

        out["contents"] = md;
        const auto wr = word_range_at(doc->text, byte_offset);
        out["range"] = range_to_json(sources.get_full_range(
          bt_dsl::SourceRange(doc->module.file_id, wr.startByte, wr.endByte)));
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
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());

    ensure_analyzed(*doc, imported_uris);

    bt_dsl::Program * p = doc->module.program;
    if (p == nullptr) {
      return out;
    }

    // Import path definition: jump to imported file root.
    for (auto * imp : p->imports()) {
      if (imp == nullptr) continue;
      const auto narrowed = narrow_to_identifier(doc->text, imp->get_range(), imp->path);
      if (!contains_byte(narrowed, byte_offset)) {
        continue;
      }
      if (auto resolved = resolve_relative_import_uri(doc->uri, imp->path)) {
        json loc;
        loc["uri"] = *resolved;
        loc["range"] = range_to_json(bt_dsl::FullSourceRange::from_byte_range(0, 0));
        out["locations"].push_back(std::move(loc));
        return out;
      }
    }

    auto push_loc = [&](
                      const std::string & target_uri, std::string_view target_text,
                      bt_dsl::SourceRange r, std::string_view ident) {
      json loc;
      loc["uri"] = target_uri;
      const auto narrowed = narrow_to_identifier(target_text, r, ident);
      bt_dsl::FullSourceRange fr = bt_dsl::FullSourceRange::from_byte_range(
        narrowed.get_begin().get_offset(), narrowed.get_end().get_offset());

      // If the target doc is in-memory, compute line/col.
      if (auto * tdoc = get_doc(target_uri)) {
        ensure_parsed(*tdoc);
        fr = sources.get_full_range(narrowed);
      }

      loc["range"] = range_to_json(fr);
      out["locations"].push_back(std::move(loc));
    };

    // Node / subtree definition
    if (auto w = word_at(doc->text, byte_offset)) {
      // Prefer same document
      for (auto * e : p->externs()) {
        if (e && e->name == *w) {
          push_loc(doc->uri, doc->text, e->get_range(), *w);
          return out;
        }
      }
      for (auto * t : p->trees()) {
        if (t && t->name == *w) {
          push_loc(doc->uri, doc->text, t->get_range(), *w);
          return out;
        }
      }

      // Then imports (public). Prefer URIs explicitly provided by the host.
      for (const auto & imp_uri : imported_uris) {
        auto * imp_doc = get_doc(imp_uri);
        if (imp_doc == nullptr) {
          continue;
        }
        ensure_parsed(*imp_doc);
        bt_dsl::Program * ip = imp_doc->module.program;
        if (ip == nullptr) {
          continue;
        }
        for (auto * e : ip->externs()) {
          if (e && e->name == *w && bt_dsl::ModuleInfo::is_public(e->name)) {
            push_loc(imp_doc->uri, imp_doc->text, e->get_range(), *w);
            return out;
          }
        }
        for (auto * t : ip->trees()) {
          if (t && t->name == *w && bt_dsl::ModuleInfo::is_public(t->name)) {
            push_loc(imp_doc->uri, imp_doc->text, t->get_range(), *w);
            return out;
          }
        }
      }
    }

    // VarRef / inline decl
    const auto hit = find_ast_hit(*p, byte_offset);
    if (hit.var_ref && hit.var_ref->resolvedSymbol) {
      push_loc(
        doc->uri, doc->text, hit.var_ref->resolvedSymbol->definitionRange, hit.var_ref->name);
      return out;
    }

    // Fallback: resolve current word in scope
    if (auto w = word_at(doc->text, byte_offset)) {
      const bt_dsl::Scope * scope =
        hit.tree ? doc->module.values.get_tree_scope(hit.tree->name) : nullptr;
      if (scope == nullptr) {
        scope = doc->module.values.get_global_scope();
      }
      if (const bt_dsl::Symbol * sym = doc->module.values.resolve(*w, scope)) {
        push_loc(doc->uri, doc->text, sym->definitionRange, sym->name);
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
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);

    auto push_sym = [&](std::string name, std::string kind, bt_dsl::SourceRange range) {
      json s;
      s["name"] = std::move(name);
      s["kind"] = std::move(kind);
      s["range"] = range_to_json(sources.get_full_range(range));
      s["selectionRange"] = range_to_json(sources.get_full_range(range));
      out["symbols"].push_back(std::move(s));
    };

    bt_dsl::Program * p = doc->module.program;
    if (p == nullptr) {
      return out;
    }

    for (auto * d0 : p->externs()) {
      if (d0) push_sym(std::string(d0->name), "Declare", d0->get_range());
    }

    for (auto * g : p->global_vars()) {
      if (g) push_sym(std::string(g->name), "GlobalVar", g->get_range());
    }

    for (auto * c : p->global_consts()) {
      if (c) push_sym(std::string(c->name), "GlobalConst", c->get_range());
    }

    for (auto * t : p->trees()) {
      if (t) push_sym(std::string(t->name), "Tree", t->get_range());
    }

    return out;
  }

  json document_highlights_json_impl(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
  {
    json out;
    out["uri"] = std::string(uri);
    out["items"] = json::array();

    auto * doc = get_doc(uri);
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);
    byte_offset = clamp_byte_offset(byte_offset, doc->text.size());
    ensure_analyzed(*doc, imported_uris);

    bt_dsl::Program * p = doc->module.program;
    if (p == nullptr) {
      return out;
    }

    const auto hit = find_ast_hit(*p, byte_offset);
    if (hit.tree == nullptr) {
      return out;
    }

    auto push_item = [&](bt_dsl::SourceRange r, std::string kind) {
      json item;
      item["range"] = range_to_json(sources.get_full_range(r));
      item["kind"] = std::move(kind);
      out["items"].push_back(std::move(item));
    };

    auto push_item_narrowed = [&](bt_dsl::SourceRange r, std::string_view ident, std::string kind) {
      push_item(narrow_to_identifier(doc->text, r, ident), std::move(kind));
    };

    // Highlight node name occurrences (node call)
    if (hit.node_stmt != nullptr) {
      const auto wr = word_range_at(doc->text, byte_offset);
      const std::string_view w =
        std::string_view(doc->text).substr(wr.startByte, wr.endByte - wr.startByte);
      if (w == hit.node_stmt->nodeName) {
        const std::string_view node_name = hit.node_stmt->nodeName;

        std::function<void(bt_dsl::Stmt *)> visit_stmt;
        std::function<void(bt_dsl::NodeStmt *)> visit_node;

        visit_node = [&](bt_dsl::NodeStmt * n) {
          if (n == nullptr) return;
          if (n->nodeName == node_name) {
            push_item_narrowed(n->get_range(), node_name, "Text");
          }
          for (auto * ch : n->children) {
            visit_stmt(ch);
          }
        };

        visit_stmt = [&](bt_dsl::Stmt * s) {
          if (auto * n = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
            visit_node(n);
          }
        };

        for (auto * stmt : hit.tree->body) {
          visit_stmt(stmt);
        }

        // Also highlight same-document decl name.
        for (auto * e : p->externs()) {
          if (e && e->name == node_name) {
            push_item_narrowed(e->get_range(), node_name, "Text");
          }
        }
        for (auto * t : p->trees()) {
          if (t && t->name == node_name) {
            push_item_narrowed(t->get_range(), node_name, "Text");
          }
        }

        return out;
      }
    }

    // Highlight symbol occurrences
    const bt_dsl::Symbol * target_sym = nullptr;
    if (hit.var_ref && hit.var_ref->resolvedSymbol) {
      target_sym = hit.var_ref->resolvedSymbol;
    }

    if (target_sym == nullptr) {
      return out;
    }

    auto kind_from_symbol = [&](const bt_dsl::Symbol * sym) {
      if (sym == nullptr) {
        return std::string("Read");
      }
      return sym->is_writable() ? std::string("Write") : std::string("Read");
    };

    if (target_sym->definitionRange.get_end().get_offset() <= doc->text.size()) {
      push_item_narrowed(target_sym->definitionRange, target_sym->name, "Write");
    }

    std::function<void(bt_dsl::Expr *, std::optional<bt_dsl::PortDirection>)> visit_expr;
    std::function<void(bt_dsl::Stmt *)> visit_stmt;

    visit_expr = [&](bt_dsl::Expr * e, std::optional<bt_dsl::PortDirection> dir) {
      if (e == nullptr) return;

      if (auto * vr = bt_dsl::dyn_cast<bt_dsl::VarRefExpr>(e)) {
        if (vr->resolvedSymbol == target_sym) {
          const std::string kind = kind_from_symbol(target_sym);
          push_item_narrowed(vr->get_range(), vr->name, kind);
        }
        return;
      }

      if (auto * b = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(e)) {
        visit_expr(b->lhs, dir);
        visit_expr(b->rhs, dir);
        return;
      }

      if (auto * u = bt_dsl::dyn_cast<bt_dsl::UnaryExpr>(e)) {
        visit_expr(u->operand, dir);
        return;
      }

      if (auto * c = bt_dsl::dyn_cast<bt_dsl::CastExpr>(e)) {
        visit_expr(c->expr, dir);
        return;
      }

      if (auto * idx = bt_dsl::dyn_cast<bt_dsl::IndexExpr>(e)) {
        visit_expr(idx->base, dir);
        visit_expr(idx->index, dir);
        return;
      }

      if (auto * arr = bt_dsl::dyn_cast<bt_dsl::ArrayLiteralExpr>(e)) {
        for (auto * el : arr->elements) {
          visit_expr(el, dir);
        }
        return;
      }

      if (auto * rep = bt_dsl::dyn_cast<bt_dsl::ArrayRepeatExpr>(e)) {
        visit_expr(rep->value, dir);
        visit_expr(rep->count, dir);
        return;
      }

      if (auto * vm = bt_dsl::dyn_cast<bt_dsl::VecMacroExpr>(e)) {
        visit_expr(vm->inner, dir);
        return;
      }
    };

    visit_stmt = [&](bt_dsl::Stmt * s) {
      if (s == nullptr) return;

      if (auto * as = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(s)) {
        if (as->resolvedTarget == target_sym) {
          push_item_narrowed(as->get_range(), as->target, "Write");
        }
        for (auto * idx : as->indices) {
          visit_expr(idx, std::nullopt);
        }
        visit_expr(as->value, std::nullopt);
        return;
      }

      if (auto * ns = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
        for (auto * pc : ns->preconditions) {
          if (pc) visit_expr(pc->condition, std::nullopt);
        }
        for (auto * arg : ns->args) {
          if (arg == nullptr) continue;
          if (arg->inlineDecl && arg->inlineDecl->name == target_sym->name) {
            push_item_narrowed(arg->inlineDecl->get_range(), arg->inlineDecl->name, "Write");
          }
          if (arg->valueExpr) {
            visit_expr(arg->valueExpr, arg->direction);
          }
        }
        for (auto * ch : ns->children) {
          visit_stmt(ch);
        }
      }
    };

    for (auto * stmt : hit.tree->body) {
      visit_stmt(stmt);
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
    if (doc == nullptr) {
      return out;
    }

    ensure_parsed(*doc);
    ensure_analyzed(*doc, imported_uris);

    bt_dsl::Program * p = doc->module.program;
    if (p == nullptr) {
      return out;
    }

    auto push_tok =
      [&](bt_dsl::SourceRange r, std::string type, const std::vector<std::string> & mods) {
        if (r.get_end().get_offset() <= r.get_begin().get_offset()) {
          return;
        }
        json t;
        t["type"] = std::move(type);
        t["modifiers"] = mods;
        t["range"] = range_to_json(sources.get_full_range(r));
        out["tokens"].push_back(std::move(t));
      };

    auto tok_ident = [&](
                       bt_dsl::SourceRange r, std::string_view ident, std::string type,
                       const std::vector<std::string> & mods) {
      push_tok(narrow_to_identifier(doc->text, r, ident), std::move(type), mods);
    };

    const std::vector<std::string> no_mods;
    const std::vector<std::string> decl_mods{"declaration"};

    // Declarations
    for (auto * e : p->externs()) {
      if (e == nullptr) continue;
      tok_ident(
        e->get_range(), e->name, token_type_for_node_category(e->category, false), decl_mods);
      for (auto * port : e->ports) {
        if (port == nullptr) continue;
        tok_ident(port->get_range(), port->name, "property", decl_mods);
      }
    }

    for (auto * t : p->trees()) {
      if (t == nullptr) continue;
      tok_ident(t->get_range(), t->name, "function", decl_mods);
      for (auto * param : t->params) {
        if (param == nullptr) continue;
        tok_ident(param->get_range(), param->name, "parameter", decl_mods);
      }
    }

    for (auto * gv : p->global_vars()) {
      if (gv == nullptr) continue;
      tok_ident(gv->get_range(), gv->name, "variable", decl_mods);
    }
    for (auto * gc : p->global_consts()) {
      if (gc == nullptr) continue;
      tok_ident(gc->get_range(), gc->name, "variable", decl_mods);
    }

    // Tree bodies: node calls + var refs
    std::function<void(bt_dsl::Expr *)> visit_expr;
    std::function<void(bt_dsl::Stmt *)> visit_stmt;

    visit_expr = [&](bt_dsl::Expr * e) {
      if (e == nullptr) return;

      if (auto * vr = bt_dsl::dyn_cast<bt_dsl::VarRefExpr>(e)) {
        tok_ident(vr->get_range(), vr->name, "variable", no_mods);
        return;
      }
      if (auto * b = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(e)) {
        visit_expr(b->lhs);
        visit_expr(b->rhs);
        return;
      }
      if (auto * u = bt_dsl::dyn_cast<bt_dsl::UnaryExpr>(e)) {
        visit_expr(u->operand);
        return;
      }
      if (auto * c = bt_dsl::dyn_cast<bt_dsl::CastExpr>(e)) {
        visit_expr(c->expr);
        return;
      }
      if (auto * idx = bt_dsl::dyn_cast<bt_dsl::IndexExpr>(e)) {
        visit_expr(idx->base);
        visit_expr(idx->index);
        return;
      }
      if (auto * arr = bt_dsl::dyn_cast<bt_dsl::ArrayLiteralExpr>(e)) {
        for (auto * el : arr->elements) {
          visit_expr(el);
        }
        return;
      }
      if (auto * rep = bt_dsl::dyn_cast<bt_dsl::ArrayRepeatExpr>(e)) {
        visit_expr(rep->value);
        visit_expr(rep->count);
        return;
      }
      if (auto * vm = bt_dsl::dyn_cast<bt_dsl::VecMacroExpr>(e)) {
        visit_expr(vm->inner);
      }
    };

    visit_stmt = [&](bt_dsl::Stmt * s) {
      if (s == nullptr) return;

      if (auto * ns = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
        const bool is_tree = ns->resolvedNode ? ns->resolvedNode->is_tree() : false;
        const auto cat =
          ns->resolvedNode ? extern_category_from_decl(ns->resolvedNode->decl) : std::nullopt;
        tok_ident(
          ns->get_range(), ns->nodeName, token_type_for_node_category(cat, is_tree), no_mods);

        for (auto * pc : ns->preconditions) {
          if (pc) visit_expr(pc->condition);
        }
        for (auto * arg : ns->args) {
          if (arg == nullptr) continue;
          if (!arg->name.empty()) {
            tok_ident(arg->get_range(), arg->name, "property", no_mods);
          }
          if (arg->inlineDecl) {
            tok_ident(arg->inlineDecl->get_range(), arg->inlineDecl->name, "variable", decl_mods);
          }
          if (arg->valueExpr) {
            visit_expr(arg->valueExpr);
          }
        }
        for (auto * ch : ns->children) {
          visit_stmt(ch);
        }
        return;
      }

      if (auto * as = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(s)) {
        tok_ident(as->get_range(), as->target, "variable", no_mods);
        for (auto * idx : as->indices) {
          visit_expr(idx);
        }
        visit_expr(as->value);
        return;
      }

      if (auto * vd = bt_dsl::dyn_cast<bt_dsl::BlackboardDeclStmt>(s)) {
        tok_ident(vd->get_range(), vd->name, "variable", decl_mods);
        if (vd->initialValue) {
          visit_expr(vd->initialValue);
        }
        return;
      }

      if (auto * cd = bt_dsl::dyn_cast<bt_dsl::ConstDeclStmt>(s)) {
        tok_ident(cd->get_range(), cd->name, "variable", decl_mods);
        visit_expr(cd->value);
        return;
      }
    };

    for (auto * t : p->trees()) {
      if (t == nullptr) continue;
      for (auto * stmt : t->body) {
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
  // Note: Document owns a TypeContext (non-copyable / non-movable). Update the
  // entry in-place.
  auto & d = impl_->docs[uri];
  d.uri = std::move(uri);
  d.text = std::move(text);
  d.module = bt_dsl::ModuleInfo{};
  d.type_ctx = std::make_unique<bt_dsl::TypeContext>();
  d.indexed = false;
  d.analyzed = false;
  d.analyzed_import_hash = 0;
  d.sema_diags = bt_dsl::DiagnosticBag{};
}

void Workspace::remove_document(std::string_view uri) { impl_->docs.erase(std::string(uri)); }

bool Workspace::has_document(std::string_view uri) const
{
  return impl_->docs.find(std::string(uri)) != impl_->docs.end();
}

std::string Workspace::diagnostics_json(std::string_view uri) { return diagnostics_json(uri, {}); }

std::string Workspace::diagnostics_json(
  std::string_view uri, const std::vector<std::string> & imported_uris)
{
  const json j = impl_->diagnostics_json_impl(uri, imported_uris);
  return j.dump();
}

std::string Workspace::resolve_imports_json(std::string_view uri, std::string_view stdlib_uri)
{
  const json j = impl_->resolve_imports_json_impl(uri, stdlib_uri);
  return j.dump();
}

std::string Workspace::completion_json(std::string_view uri, uint32_t byte_offset)
{
  return completion_json(uri, byte_offset, {}, {});
}

std::string Workspace::completion_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris,
  std::string_view trigger)
{
  const json j = impl_->completion_json_impl(uri, byte_offset, imported_uris, trigger);
  return j.dump();
}

std::string Workspace::hover_json(std::string_view uri, uint32_t byte_offset)
{
  return hover_json(uri, byte_offset, {});
}

std::string Workspace::hover_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  const json j = impl_->hover_json_impl(uri, byte_offset, imported_uris);
  return j.dump();
}

std::string Workspace::definition_json(std::string_view uri, uint32_t byte_offset)
{
  return definition_json(uri, byte_offset, {});
}

std::string Workspace::definition_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  const json j = impl_->definition_json_impl(uri, byte_offset, imported_uris);
  return j.dump();
}

std::string Workspace::document_symbols_json(std::string_view uri)
{
  const json j = impl_->document_symbols_json_impl(uri);
  return j.dump();
}

std::string Workspace::document_highlights_json(std::string_view uri, uint32_t byte_offset)
{
  return document_highlights_json(uri, byte_offset, {});
}

std::string Workspace::document_highlights_json(
  std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris)
{
  const json j = impl_->document_highlights_json_impl(uri, byte_offset, imported_uris);
  return j.dump();
}

std::string Workspace::semantic_tokens_json(std::string_view uri)
{
  return semantic_tokens_json(uri, {});
}

std::string Workspace::semantic_tokens_json(
  std::string_view uri, const std::vector<std::string> & imported_uris)
{
  const json j = impl_->semantic_tokens_json_impl(uri, imported_uris);
  return j.dump();
}

}  // namespace bt_dsl::lsp
