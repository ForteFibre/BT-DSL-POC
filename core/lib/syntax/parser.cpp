#include "bt_dsl/syntax/parser.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace bt_dsl::syntax
{
namespace
{

[[nodiscard]] bool is_common_unsupported_keyword(std::string_view ident) noexcept
{
  // Intentionally *not* treated as reserved identifiers everywhere;
  // we only special-case them in statement / top-level positions to provide
  // more helpful diagnostics.
  static const std::string_view k_unsupported[] = {
    "let", "def", "func", "class", "if", "else", "while", "for", "return", "switch", "match"};
  return std::any_of(std::begin(k_unsupported), std::end(k_unsupported), [&](std::string_view k) {
    return ident == k;
  });
}

[[nodiscard]] std::string unsupported_keyword_message(std::string_view ident, bool top_level)
{
  if (ident == "let") {
    return "unsupported keyword 'let'";
  }
  if (ident == "def" || ident == "func") {
    return std::string("unsupported keyword '") + std::string(ident) +
           "' (BT-DSL does not support function declarations)";
  }
  if (ident == "class") {
    return "unsupported keyword 'class' (BT-DSL does not support class declarations)";
  }
  if (ident == "if" || ident == "else") {
    return "unsupported imperative control flow '" + std::string(ident) + "'";
  }
  if (ident == "while" || ident == "for") {
    return "unsupported imperative loop '" + std::string(ident) + "'";
  }
  if (ident == "return") {
    return "unsupported keyword 'return'";
  }
  (void)top_level;
  return std::string("unsupported keyword '") + std::string(ident) + "'";
}

[[nodiscard]] std::string unsupported_keyword_hint(std::string_view ident, bool top_level)
{
  if (ident == "let") {
    return "Use 'var' (mutable) or 'const' (immutable) instead.";
  }
  if (ident == "def" || ident == "func") {
    if (top_level) {
      return "Use 'tree' to define a behavior tree, or 'extern' to declare nodes.";
    }
    return "BT-DSL does not have functions; inside a tree body use node calls, 'var', 'const', or "
           "assignments.";
  }
  if (ident == "class") {
    if (top_level) {
      return "Use 'extern type' for external types, or 'type' for type aliases.";
    }
    return "Use type names and 'type' aliases; classes are not part of BT-DSL.";
  }
  if (ident == "if" || ident == "else") {
    return "BT-DSL is declarative. Use Sequence/Selector nodes for control flow, or decorators "
           "like "
           "ForceSuccess/ForceFailure.";
  }
  if (ident == "while" || ident == "for") {
    return "BT-DSL does not support imperative loops. Use the Repeat decorator, RunWhile "
           "precondition, or behavior tree structure to control execution loops.";
  }
  if (ident == "return") {
    return "Behavior trees return status (Success/Failure/Running) automatically based on child "
           "execution.";
  }
  if (ident == "switch" || ident == "match") {
    return "Use a Selector node with condition checks to implement branching logic.";
  }
  return "";
}

SourceRange join_ranges(SourceRange a, SourceRange b)
{
  if (a.is_invalid()) return b;
  if (b.is_invalid()) return a;
  // Ranges are expected to be in the same file.
  return {a.get_begin(), b.get_end()};
}

void append_utf8(std::string & out, uint32_t cp)
{
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
    return;
  }
  if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return;
  }
  if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    return;
  }
  out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
  out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

}  // namespace

const Token & Parser::cur(size_t lookahead) const
{
  const size_t i = idx_ + lookahead;
  if (i >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[i];
}

bool Parser::at(TokenKind k) const { return cur().kind == k; }

bool Parser::at_eof() const { return at(TokenKind::Eof); }

const Token & Parser::advance()
{
  const Token & t = cur();
  if (!at_eof()) {
    ++idx_;
  }
  return t;
}

bool Parser::match(TokenKind k)
{
  if (at(k)) {
    advance();
    return true;
  }
  return false;
}

bool Parser::expect(TokenKind k, std::string_view what, RecoverySet recovery)
{
  if (match(k)) {
    return true;
  }

  // Improved error location for missing semicolons:
  // If we expect a semicolon but find a token on a new line, report it at the end of the previous line.
  if (k == TokenKind::Semicolon && idx_ > 0) {
    const Token & prev = tokens_[idx_ - 1];
    const Token & curr = cur();
    // Only if we haven't reached EOF (or check valid ranges)
    if (prev.range.is_valid() && curr.range.is_valid()) {
      auto prev_lc = source_.get_line_column(prev.range.get_end().offset());
      auto curr_lc = source_.get_line_column(curr.range.get_begin().offset());
      if (curr_lc.line > prev_lc.line) {
        diags_
          .report_error(prev.range, std::string("expected ") + std::string(what), "expected `;`")
          .with_fixit(prev.range, ";");
        goto recovery;
      }
    }
  }

  // Provide help message for missing semicolons
  if (k == TokenKind::Semicolon) {
    diags_.report_error(cur().range, std::string("expected ") + std::string(what), "expected `;`")
      .with_fixit(cur().range, ";");
  } else {
    error_at(cur(), std::string("expected ") + std::string(what));
  }

recovery:
  if (recovery != RecoverySet::None) {
    while (!at_eof()) {
      if (at(k)) {
        return true;
      }

      const TokenKind kind = cur().kind;

      // Common stop tokens
      if (
        kind == TokenKind::Semicolon &&
        (recovery & RecoverySet::Statement || recovery & RecoverySet::Block ||
         recovery & RecoverySet::Argument)) {
        return false;
      }
      if (
        kind == TokenKind::RBrace &&
        (recovery & RecoverySet::Block || recovery & RecoverySet::Argument)) {
        return false;
      }
      if (kind == TokenKind::RParen && (recovery & RecoverySet::Argument)) {
        return false;
      }
      if (kind == TokenKind::LBrace && (recovery & RecoverySet::Argument)) {
        // If we see a {, we might have missed the closing ) of args.
        // Stop consuming so valid block can be parsed.
        return false;
      }

      // Stop at new statement keywords if recovering statement/block
      if (
        (recovery & RecoverySet::Statement || recovery & RecoverySet::Block) &&
        (is_kw("var", cur()) || is_kw("const", cur()) || is_kw("type", cur()) ||
         is_kw("tree", cur()) || is_kw("import", cur()) || is_kw("extern", cur()))) {
        return false;
      }

      advance();
    }
  }

  return false;
}

void Parser::error_at(const Token & t, std::string_view msg)
{
  // Point at current token if possible; otherwise at end-of-file.
  diags_.report_error(t.range, std::string(msg));
}

void Parser::synchronize_to_stmt()
{
  while (!at_eof()) {
    if (match(TokenKind::Semicolon)) {
      return;
    }
    // Also stop at tokens that can start a new statement or close a block.
    if (at(TokenKind::RBrace)) {
      return;
    }
    // Stop at keywords that begin new declarations/statements.
    if (
      is_kw("var", cur()) || is_kw("const", cur()) || is_kw("type", cur()) ||
      is_kw("tree", cur()) || is_kw("import", cur()) || is_kw("extern", cur())) {
      return;
    }
    advance();
  }
}

void Parser::synchronize_skip_block()
{
  // Skip balanced {} blocks to reduce cascading errors.
  // Useful when encountering unsupported control flow like `if (...) { ... }`.
  int brace_depth = 0;

  while (!at_eof()) {
    if (at(TokenKind::LBrace)) {
      ++brace_depth;
      advance();
      continue;
    }

    if (at(TokenKind::RBrace)) {
      if (brace_depth > 0) {
        --brace_depth;
        advance();
        if (brace_depth == 0) {
          // Successfully consumed balanced block
          return;
        }
        continue;
      }
      // Unmatched '}' - stop without consuming
      return;
    }

    if (brace_depth == 0) {
      // Not inside a block - use normal statement sync
      if (match(TokenKind::Semicolon)) {
        return;
      }
      if (
        is_kw("var", cur()) || is_kw("const", cur()) || is_kw("type", cur()) ||
        is_kw("tree", cur()) || is_kw("import", cur()) || is_kw("extern", cur())) {
        return;
      }
    }

    advance();
  }
}

bool Parser::is_kw(std::string_view kw, const Token & t)
{
  return t.kind == TokenKind::Identifier && t.text == kw;
}

bool Parser::is_reserved_ident(std::string_view ident)
{
  // Keep this list aligned with reference tests (lexical structure).
  // These tokens are lexed as identifiers, but treated as keywords by the parser.
  static constexpr std::string_view k_reserved[] = {
    "import",
    "extern",
    "type",
    "var",
    "const",
    "tree",
    "true",
    "false",
    "null",
    "action",
    "condition",
    "control",
    "decorator",
    "subtree",
    // Also reserve grammar-significant identifiers.
    "in",
    "out",
    "ref",
    "mut",
    "as",
  };

  return std::any_of(
    std::begin(k_reserved), std::end(k_reserved), [&](std::string_view kw) { return ident == kw; });
}

bool Parser::expect_identifier_not_reserved(std::string_view what)
{
  const Token & t = cur();
  if (t.kind != TokenKind::Identifier) {
    error_at(t, std::string("expected ") + std::string(what));
    return false;
  }

  bool ok = true;
  if (is_reserved_ident(t.text)) {
    error_at(t, "keyword cannot be used as identifier");
    ok = false;
  }

  // Always consume one token so parsing can continue.
  advance();
  return ok;
}

std::optional<PortDirection> Parser::parse_port_direction_opt()
{
  if (cur().kind != TokenKind::Identifier) {
    return std::nullopt;
  }
  if (cur().text == "in") {
    advance();
    return PortDirection::In;
  }
  if (cur().text == "out") {
    advance();
    return PortDirection::Out;
  }
  if (cur().text == "ref") {
    advance();
    return PortDirection::Ref;
  }
  if (cur().text == "mut") {
    advance();
    return PortDirection::Mut;
  }
  return std::nullopt;
}

std::vector<std::string_view> Parser::collect_module_docs()
{
  std::vector<std::string_view> docs;
  while (at(TokenKind::DocModule)) {
    docs.push_back(ast_.intern(cur().text));
    advance();
  }
  return docs;
}

std::vector<std::string_view> Parser::collect_line_docs()
{
  std::vector<std::string_view> docs;
  while (at(TokenKind::DocLine)) {
    docs.push_back(ast_.intern(cur().text));
    advance();
  }
  return docs;
}

std::vector<Precondition *> Parser::collect_preconditions()
{
  std::vector<Precondition *> out;
  while (match(TokenKind::At)) {
    const Token & kind_tok = cur();
    if (!expect(TokenKind::Identifier, "precondition kind")) {
      // skip until end of line / next statement boundary
      synchronize_to_stmt();
      break;
    }

    PreconditionKind pk = PreconditionKind::Guard;
    if (kind_tok.text == "success_if") {
      pk = PreconditionKind::SuccessIf;
    } else if (kind_tok.text == "failure_if") {
      pk = PreconditionKind::FailureIf;
    } else if (kind_tok.text == "skip_if") {
      pk = PreconditionKind::SkipIf;
    } else if (kind_tok.text == "run_while") {
      pk = PreconditionKind::RunWhile;
    } else if (kind_tok.text == "guard") {
      pk = PreconditionKind::Guard;
    } else {
      error_at(kind_tok, "unknown precondition kind");
    }

    expect(TokenKind::LParen, "'(' after precondition");
    Expr * cond = parse_expr();
    expect(TokenKind::RParen, "')' after precondition condition");

    const SourceRange r = join_ranges(kind_tok.range, cur().range);
    out.push_back(ast_.create<Precondition>(pk, cond, r));
  }
  return out;
}

Program * Parser::parse_program()
{
  auto * prog =
    ast_.create<Program>(SourceRange(file_id_, 0, static_cast<uint32_t>(source_.size())));

  // Module docs at the very beginning
  {
    auto md = collect_module_docs();
    prog->innerDocs = ast_.copy_to_arena(md);
  }

  std::vector<Decl *> decls;

  while (!at_eof()) {
    const auto docs = collect_line_docs();

    if (is_kw("import", cur())) {
      decls.push_back(parse_import_decl(docs));
      continue;
    }

    // Attribute-prefixed top-level decls
    if (at(TokenKind::Hash)) {
      auto * attr = parse_behavior_attr_opt();
      if (is_kw("extern", cur())) {
        decls.push_back(parse_extern_decl(docs, attr));
      } else {
        error_at(cur(), "unexpected attribute on this declaration (only valid on extern)");
        // Continue to main loop to parse the underlying declaration (e.g. tree)
        // treating the attribute as if it was ignored.
      }
      continue;
    }

    if (is_kw("extern", cur())) {
      // Need to decide between extern type and extern node
      // Lookahead: extern type
      if (cur(1).kind == TokenKind::Identifier && cur(1).text == "type") {
        decls.push_back(parse_extern_type_decl(docs));
      } else {
        decls.push_back(parse_extern_decl(docs));
      }
      continue;
    }

    if (is_kw("type", cur())) {
      decls.push_back(parse_type_alias_decl(docs));
      continue;
    }

    if (is_kw("var", cur())) {
      decls.push_back(parse_global_var_decl(docs));
      continue;
    }

    if (is_kw("const", cur())) {
      decls.push_back(parse_global_const_decl(docs));
      continue;
    }

    if (is_kw("tree", cur())) {
      decls.push_back(parse_tree_decl(docs));
      continue;
    }

    // Helpful diagnostics for common-but-unsupported keywords.
    if (cur().kind == TokenKind::Identifier && is_common_unsupported_keyword(cur().text)) {
      const Token & t = cur();
      const std::string hint = unsupported_keyword_hint(t.text, /*top_level=*/true);
      diags_.report_error(t.range, unsupported_keyword_message(t.text, /*top_level=*/true))
        .with_help(hint);
      advance();
      // Use block-aware recovery to skip any associated {} block.
      synchronize_skip_block();
      continue;
    }

    // Unexpected token at top level.
    if (!docs.empty()) {
      // If docs exist but no decl follows, keep going.
    }
    error_at(cur(), "unexpected token at top-level");
    advance();
    synchronize_to_stmt();
  }

  prog->decls = ast_.copy_to_arena(decls);

  return prog;
}

ImportDecl * Parser::parse_import_decl(const std::vector<std::string_view> & /*docs*/)
{
  const Token & kw = advance();
  (void)kw;

  const Token & path_tok = cur();
  if (!expect(TokenKind::StringLiteral, "string literal import path")) {
    synchronize_to_stmt();
    return ast_.create<ImportDecl>(
      ast_.intern(""), SourceRange(file_id_, path_tok.begin(), path_tok.end()));
  }

  const std::string unescaped = unescape_string(path_tok.text, path_tok);
  auto * decl =
    ast_.create<ImportDecl>(ast_.intern(unescaped), join_ranges(kw.range, path_tok.range));

  if (expect(TokenKind::Semicolon, "';' after import")) {
    const Token & semi_tok = tokens_[idx_ - 1];
    decl->range_ = join_ranges(decl->get_range(), semi_tok.range);
  }
  return decl;
}

ExternTypeDecl * Parser::parse_extern_type_decl(const std::vector<std::string_view> & docs)
{
  const Token & kw = advance();
  (void)kw;
  advance();  // 'type'

  const Token & name_tok = cur();
  if (!expect_identifier_not_reserved("extern type name")) {
    synchronize_to_stmt();
    auto * d = ast_.create<ExternTypeDecl>(ast_.intern(""), name_tok.range);
    d->docs = ast_.copy_to_arena(docs);
    return d;
  }

  auto * d =
    ast_.create<ExternTypeDecl>(ast_.intern(name_tok.text), join_ranges(kw.range, name_tok.range));
  d->docs = ast_.copy_to_arena(docs);

  if (expect(TokenKind::Semicolon, "';' after extern type")) {
    const Token & semi_tok = tokens_[idx_ - 1];
    d->range_ = join_ranges(d->get_range(), semi_tok.range);
  }
  return d;
}

TypeAliasDecl * Parser::parse_type_alias_decl(const std::vector<std::string_view> & docs)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("type alias name");

  expect(TokenKind::Eq, "'=' in type alias");
  TypeExpr * ty = parse_type_expr();

  expect(TokenKind::Semicolon, "';' after type alias");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * d = ast_.create<TypeAliasDecl>(
    ast_.intern(name_tok.text), ty, join_ranges(kw.range, semi_tok.range));
  d->docs = ast_.copy_to_arena(docs);
  return d;
}

GlobalVarDecl * Parser::parse_global_var_decl(const std::vector<std::string_view> & docs)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("global var name");

  TypeExpr * ty = nullptr;
  Expr * init = nullptr;

  if (match(TokenKind::Colon)) {
    ty = parse_type_expr();
  }

  if (match(TokenKind::Eq)) {
    init = parse_expr();
  }

  expect(TokenKind::Semicolon, "';' after global var");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * d = ast_.create<GlobalVarDecl>(
    ast_.intern(name_tok.text), ty, init, join_ranges(kw.range, semi_tok.range));
  d->docs = ast_.copy_to_arena(docs);
  return d;
}

GlobalConstDecl * Parser::parse_global_const_decl(const std::vector<std::string_view> & docs)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("global const name");

  TypeExpr * ty = nullptr;

  if (match(TokenKind::Colon)) {
    ty = parse_type_expr();
  }

  expect(TokenKind::Eq, "'=' in global const");
  Expr * value = parse_expr();

  expect(TokenKind::Semicolon, "';' after global const");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * d = ast_.create<GlobalConstDecl>(
    ast_.intern(name_tok.text), ty, value, join_ranges(kw.range, semi_tok.range));
  d->docs = ast_.copy_to_arena(docs);
  return d;
}

BehaviorAttr * Parser::parse_behavior_attr_opt()
{
  if (!match(TokenKind::Hash)) {
    return nullptr;
  }
  if (!expect(TokenKind::LBracket, "'[' after #")) {
    return nullptr;
  }

  const Token & beh = cur();
  if (!expect(TokenKind::Identifier, "attribute name")) {
    return nullptr;
  }
  if (beh.text != "behavior") {
    error_at(beh, "unknown attribute (expected behavior)");
  }

  expect(TokenKind::LParen, "'(' after behavior");

  // DataPolicy
  DataPolicy dp = DataPolicy::Any;
  {
    const Token & dp_tok = cur();
    expect(TokenKind::Identifier, "data policy");
    if (dp_tok.text == "All") {
      dp = DataPolicy::All;
    } else if (dp_tok.text == "Any") {
      dp = DataPolicy::Any;
    } else if (dp_tok.text == "None") {
      dp = DataPolicy::None;
    } else {
      error_at(dp_tok, "unknown data policy");
    }
  }

  std::optional<FlowPolicy> fp;
  if (match(TokenKind::Comma)) {
    const Token & fp_tok = cur();
    expect(TokenKind::Identifier, "flow policy");
    if (fp_tok.text == "Chained") {
      fp = FlowPolicy::Chained;
    } else if (fp_tok.text == "Isolated") {
      fp = FlowPolicy::Isolated;
    } else {
      error_at(fp_tok, "unknown flow policy");
    }
  }

  expect(TokenKind::RParen, "')' after behavior args");
  expect(TokenKind::RBracket, "']' after attribute");

  auto * a = ast_.create<BehaviorAttr>(dp, fp, join_ranges(beh.range, cur().range));
  return a;
}

ExternDecl * Parser::parse_extern_decl(
  const std::vector<std::string_view> & docs, BehaviorAttr * pre_attr)
{
  BehaviorAttr * attr = pre_attr;
  if (attr == nullptr) {
    attr = parse_behavior_attr_opt();
  }

  const Token & kw = cur();
  expect(TokenKind::Identifier, "extern");

  // category
  const Token & cat_tok = cur();
  expect(TokenKind::Identifier, "extern category");
  ExternNodeCategory cat = ExternNodeCategory::Action;
  if (cat_tok.text == "action") {
    cat = ExternNodeCategory::Action;
  } else if (cat_tok.text == "condition") {
    cat = ExternNodeCategory::Condition;
  } else if (cat_tok.text == "control") {
    cat = ExternNodeCategory::Control;
  } else if (cat_tok.text == "decorator") {
    cat = ExternNodeCategory::Decorator;
  } else if (cat_tok.text == "subtree") {
    cat = ExternNodeCategory::Subtree;
  } else {
    error_at(cat_tok, "unknown extern category");
  }

  const Token & name_tok = cur();
  expect_identifier_not_reserved("extern name");

  auto * d =
    ast_.create<ExternDecl>(cat, ast_.intern(name_tok.text), join_ranges(kw.range, name_tok.range));
  d->docs = ast_.copy_to_arena(docs);
  d->behaviorAttr = attr;

  expect(TokenKind::LParen, "'(' after extern name");

  std::vector<ExternPort *> ports;
  if (!at(TokenKind::RParen)) {
    while (true) {
      auto port_docs = collect_line_docs();
      if (at(TokenKind::RParen)) {
        // Allow trailing doc comments before ')'.
        break;
      }
      ExternPort * p = parse_extern_port();
      if (p != nullptr) {
        p->docs = ast_.copy_to_arena(port_docs);
        ports.push_back(p);
      }
      if (match(TokenKind::Comma)) {
        if (at(TokenKind::RParen)) {
          break;
        }
        continue;
      }
      break;
    }
  }

  expect(TokenKind::RParen, "')' after extern ports");
  if (expect(TokenKind::Semicolon, "';' after extern")) {
    const Token & semi_tok = tokens_[idx_ - 1];
    d->range_ = join_ranges(d->get_range(), semi_tok.range);
  }

  d->ports = ast_.copy_to_arena(ports);
  return d;
}

TreeDecl * Parser::parse_tree_decl(const std::vector<std::string_view> & docs)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("tree name");

  auto * t =
    ast_.create<TreeDecl>(ast_.intern(name_tok.text), join_ranges(kw.range, name_tok.range));
  t->docs = ast_.copy_to_arena(docs);

  // Tree parameter list is mandatory, but we can recover nicely when it's missing.
  // Example: `tree Main { ... }` -> suggest `tree Main() { ... }` and continue.
  const bool has_parens = match(TokenKind::LParen);
  if (!has_parens) {
    if (at(TokenKind::LBrace)) {
      error_at(cur(), "tree parameters '()' are required after the tree name");
      diags_.report_hint(cur().range, "Write: tree Main() { ... }");
      // Treat as empty parameter list and continue.
    } else {
      expect(TokenKind::LParen, "'(' after tree name");
      // Try to recover to '{' to avoid cascading errors.
      while (!at_eof() && !at(TokenKind::LBrace) && !at(TokenKind::Semicolon)) {
        // If we see a clear new top-level decl start, stop.
        if (
          is_kw("import", cur()) || is_kw("extern", cur()) || is_kw("type", cur()) ||
          is_kw("var", cur()) || is_kw("const", cur()) || is_kw("tree", cur())) {
          break;
        }
        advance();
      }
    }
  }
  std::vector<ParamDecl *> params;
  if (has_parens) {
    if (!at(TokenKind::RParen)) {
      while (true) {
        params.push_back(parse_param_decl());
        if (match(TokenKind::Comma)) {
          if (at(TokenKind::RParen)) break;
          continue;
        }
        break;
      }
    }
    expect(TokenKind::RParen, "')' after tree params");
  }

  expect(TokenKind::LBrace, "'{' to start tree body");

  // Body
  std::vector<Stmt *> body;
  while (!at_eof() && !at(TokenKind::RBrace)) {
    // Check for top-level keyword recovery (missing '}')
    const Token & tok = cur();
    if (
      is_kw("tree", tok) || is_kw("import", tok) || is_kw("extern", tok) || is_kw("type", tok) ||
      (is_kw("const", tok) && is_kw("var", cur(2)))) {  // heuristic mostly for pure top-level
      // Just check the definitive top-level ones.
      // const and var are valid in bodies, so don't recover on them easily unless we are sure.
    }
    if (is_kw("tree", tok) || is_kw("import", tok) || is_kw("extern", tok) || is_kw("type", tok)) {
      break;
    }

    Stmt * st = parse_stmt();
    if (st != nullptr) {
      body.push_back(st);
    }
  }
  const Token & rbrace = cur();
  expect(TokenKind::RBrace, "'}' to end tree");

  t->params = ast_.copy_to_arena(params);
  t->body = ast_.copy_to_arena(body);
  t->range_ = join_ranges(kw.range, rbrace.range);

  return t;
}

Stmt * Parser::parse_stmt()
{
  const auto docs = collect_line_docs();
  const auto preconds = collect_preconditions();

  // More helpful messages for common unsupported keywords used as statement starters.
  if (cur().kind == TokenKind::Identifier && is_common_unsupported_keyword(cur().text)) {
    const Token & t = cur();
    const std::string hint = unsupported_keyword_hint(t.text, /*top_level=*/false);
    diags_.report_error(t.range, unsupported_keyword_message(t.text, /*top_level=*/false))
      .with_help(hint);
    advance();
    // Use block-aware recovery to skip any associated {} block.
    synchronize_skip_block();
    return nullptr;
  }

  if (is_kw("var", cur())) {
    return parse_var_stmt(docs, preconds);
  }
  if (is_kw("const", cur())) {
    return parse_const_stmt(docs, preconds);
  }

  // Either assignment or node call. Both start with identifier.
  if (cur().kind == TokenKind::Identifier) {
    // Lookahead for assignment operators after optional indices.
    // We'll parse as assignment if we can find an assign op following ident/indexes.
    // Otherwise treat as node stmt.

    // Quick heuristic: if next non-bracket token is assignment op.
    size_t look = 1;
    if (cur(look).kind == TokenKind::LBracket) {
      // skip [...]*
      while (cur(look).kind == TokenKind::LBracket) {
        // find matching ]
        int depth = 0;
        while (true) {
          const TokenKind k = cur(look).kind;
          if (k == TokenKind::LBracket) depth++;
          if (k == TokenKind::RBracket) {
            depth--;
            if (depth == 0) {
              ++look;
              break;
            }
          }
          if (k == TokenKind::Eof) break;
          ++look;
        }
      }
    }

    const TokenKind after = cur(look).kind;
    if (
      after == TokenKind::Eq || after == TokenKind::PlusEq || after == TokenKind::MinusEq ||
      after == TokenKind::StarEq || after == TokenKind::SlashEq || after == TokenKind::PercentEq) {
      return parse_assignment_stmt(docs, preconds);
    }

    return parse_node_stmt(docs, preconds);
  }

  error_at(cur(), "expected statement");
  // Ensure progress before recovery to avoid repeating the same error.
  if (!at_eof()) {
    advance();
  }
  synchronize_to_stmt();
  return nullptr;
}

BlackboardDeclStmt * Parser::parse_var_stmt(
  const std::vector<std::string_view> & docs, const std::vector<Precondition *> & /*preconds*/)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("variable name");

  TypeExpr * ty = nullptr;
  Expr * init = nullptr;

  if (match(TokenKind::Colon)) {
    ty = parse_type_expr();
  }
  if (match(TokenKind::Eq)) {
    init = parse_expr();
  }

  expect(TokenKind::Semicolon, "';' after var decl");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * st = ast_.create<BlackboardDeclStmt>(
    ast_.intern(name_tok.text), ty, init, join_ranges(kw.range, semi_tok.range));
  st->docs = ast_.copy_to_arena(docs);
  return st;
}

ConstDeclStmt * Parser::parse_const_stmt(
  const std::vector<std::string_view> & docs, const std::vector<Precondition *> & /*preconds*/)
{
  const Token & kw = advance();
  const Token & name_tok = cur();
  expect_identifier_not_reserved("const name");

  TypeExpr * ty = nullptr;
  if (match(TokenKind::Colon)) {
    ty = parse_type_expr();
  }

  expect(TokenKind::Eq, "'=' in const decl");
  Expr * value = parse_expr();

  expect(TokenKind::Semicolon, "';' after const decl");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * st = ast_.create<ConstDeclStmt>(
    ast_.intern(name_tok.text), ty, value, join_ranges(kw.range, semi_tok.range));
  st->docs = ast_.copy_to_arena(docs);
  return st;
}

AssignmentStmt * Parser::parse_assignment_stmt(
  const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds)
{
  const Token & name_tok = cur();
  expect_identifier_not_reserved("assignment target");

  std::vector<Expr *> indices;
  while (match(TokenKind::LBracket)) {
    Expr * idx = parse_expr();
    expect(TokenKind::RBracket, "']' after index");
    indices.push_back(idx);
  }

  AssignOp op = AssignOp::Assign;
  const Token & op_tok = cur();
  if (match(TokenKind::Eq)) {
    op = AssignOp::Assign;
  } else if (match(TokenKind::PlusEq)) {
    op = AssignOp::AddAssign;
  } else if (match(TokenKind::MinusEq)) {
    op = AssignOp::SubAssign;
  } else if (match(TokenKind::StarEq)) {
    op = AssignOp::MulAssign;
  } else if (match(TokenKind::SlashEq)) {
    op = AssignOp::DivAssign;
  } else if (match(TokenKind::PercentEq)) {
    op = AssignOp::ModAssign;
  } else {
    error_at(op_tok, "expected assignment operator");
    if (!at_eof()) {
      advance();
    }
  }

  Expr * value = parse_expr();
  expect(TokenKind::Semicolon, "';' after assignment");

  const Token & semi_tok = tokens_[idx_ - 1];

  auto * st = ast_.create<AssignmentStmt>(
    ast_.intern(name_tok.text), op, value, join_ranges(name_tok.range, semi_tok.range));
  st->docs = ast_.copy_to_arena(docs);
  st->preconditions = ast_.copy_to_arena(preconds);
  st->indices = ast_.copy_to_arena(indices);
  return st;
}

NodeStmt * Parser::parse_node_stmt(
  const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds)
{
  const Token & name_tok = cur();
  expect_identifier_not_reserved("node name");

  auto * st = ast_.create<NodeStmt>(ast_.intern(name_tok.text), name_tok.range);
  st->docs = ast_.copy_to_arena(docs);
  st->preconditions = ast_.copy_to_arena(preconds);

  std::vector<Argument *> args;

  // Leaf: Name(...);
  // Compound: Name { ... }
  if (match(TokenKind::LParen)) {
    st->hasPropertyBlock = true;
    if (!at(TokenKind::RParen)) {
      while (true) {
        args.push_back(parse_argument());
        if (match(TokenKind::Comma)) {
          if (at(TokenKind::RParen)) break;
          continue;
        }
        break;
      }
    }
    expect(TokenKind::RParen, "')' after args", RecoverySet::Argument);
  }

  st->args = ast_.copy_to_arena(args);

  // Children block
  if (match(TokenKind::LBrace)) {
    st->hasChildrenBlock = true;

    std::vector<Stmt *> children;
    while (!at_eof() && !at(TokenKind::RBrace)) {
      // Check for top-level keyword recovery (missing '}')
      const Token & t = cur();
      if (is_kw("tree", t) || is_kw("import", t) || is_kw("extern", t) || is_kw("type", t)) {
        error_at(t, "expected '}' to end children block");
        break;
      }

      Stmt * child = parse_stmt();
      if (child) {
        children.push_back(child);
      }
    }

    const Token & rb = cur();
    expect(TokenKind::RBrace, "'}' after children block");

    st->children = ast_.copy_to_arena(children);
    st->range_ = join_ranges(name_tok.range, rb.range);
    return st;
  }

  // Leaf call requires semicolon
  expect(TokenKind::Semicolon, "';' after node call");
  {
    const Token & semi_tok = tokens_[idx_ - 1];
    st->range_ = join_ranges(name_tok.range, semi_tok.range);
  }
  return st;
}

Argument * Parser::parse_argument()
{
  const Token & name_tok = cur();
  expect_identifier_not_reserved("argument name");
  expect(TokenKind::Colon, "':' after argument name");

  auto dir = parse_port_direction_opt();

  // Inline decl: out var name
  if (dir && *dir == PortDirection::Out && is_kw("var", cur())) {
    InlineBlackboardDecl * decl = parse_inline_blackboard_decl();
    return ast_.create<Argument>(
      ast_.intern(name_tok.text), decl, join_ranges(name_tok.range, decl->get_range()));
  }

  Expr * val = parse_expr();
  if (dir) {
    return ast_.create<Argument>(
      ast_.intern(name_tok.text), dir, val, join_ranges(name_tok.range, val->get_range()));
  }
  return ast_.create<Argument>(
    ast_.intern(name_tok.text), val, join_ranges(name_tok.range, val->get_range()));
}

InlineBlackboardDecl * Parser::parse_inline_blackboard_decl()
{
  // expects current is 'var' (direction already consumed)
  const Token & var_kw = advance();
  (void)var_kw;
  const Token & name_tok = cur();
  expect_identifier_not_reserved("inline var name");
  return ast_.create<InlineBlackboardDecl>(ast_.intern(name_tok.text), name_tok.range);
}

ParamDecl * Parser::parse_param_decl()
{
  auto dir = parse_port_direction_opt();

  const Token & name_tok = cur();
  expect_identifier_not_reserved("param name");
  expect(TokenKind::Colon, "':' after param name");
  TypeExpr * ty = parse_type_expr();

  Expr * def = nullptr;
  if (match(TokenKind::Eq)) {
    if (
      dir &&
      (*dir == PortDirection::Ref || *dir == PortDirection::Out || *dir == PortDirection::Mut)) {
      error_at(tokens_[idx_ - 1], "default value is not allowed for ref/out/mut ports");
    }
    def = parse_expr();
  }

  if (dir || def) {
    return ast_.create<ParamDecl>(
      ast_.intern(name_tok.text), dir, ty, def, join_ranges(name_tok.range, ty->get_range()));
  }
  return ast_.create<ParamDecl>(
    ast_.intern(name_tok.text), ty, join_ranges(name_tok.range, ty->get_range()));
}

ExternPort * Parser::parse_extern_port()
{
  auto dir = parse_port_direction_opt();

  const Token & name_tok = cur();
  expect_identifier_not_reserved("port name");
  expect(TokenKind::Colon, "':' after port name");
  TypeExpr * ty = parse_type_expr();

  Expr * def = nullptr;
  if (match(TokenKind::Eq)) {
    if (
      dir &&
      (*dir == PortDirection::Ref || *dir == PortDirection::Out || *dir == PortDirection::Mut)) {
      error_at(tokens_[idx_ - 1], "default value is not allowed for ref/out/mut ports");
    }
    def = parse_expr();
  }

  if (dir || def) {
    return ast_.create<ExternPort>(
      ast_.intern(name_tok.text), dir, ty, def, join_ranges(name_tok.range, ty->get_range()));
  }
  return ast_.create<ExternPort>(
    ast_.intern(name_tok.text), ty, join_ranges(name_tok.range, ty->get_range()));
}

TypeExpr * Parser::parse_type_expr()
{
  const Token & start_tok = cur();
  TypeNode * base = parse_type_base();
  bool nullable = false;
  if (match(TokenKind::Question)) {
    nullable = true;
  }
  const Token & end_tok = cur();
  return ast_.create<TypeExpr>(base, nullable, join_ranges(start_tok.range, end_tok.range));
}

TypeNode * Parser::parse_type_base()
{
  const Token & t = cur();

  if (t.kind == TokenKind::Identifier && t.text == "_") {
    advance();
    return ast_.create<InferType>(t.range);
  }

  if (t.kind == TokenKind::Identifier && t.text == "vec") {
    advance();
    expect(TokenKind::Lt, "'<' after vec");
    TypeExpr * elem = parse_type_expr();
    expect(TokenKind::Gt, "'>' after vec element type");
    return ast_.create<DynamicArrayType>(elem, join_ranges(t.range, elem->get_range()));
  }

  if (match(TokenKind::LBracket)) {
    TypeExpr * elem = parse_type_expr();
    expect(TokenKind::Semicolon, "';' in array type");
    bool bounded = false;
    if (match(TokenKind::Le)) {
      bounded = true;
    }
    const Token & size_tok = cur();
    if (size_tok.kind == TokenKind::IntLiteral || size_tok.kind == TokenKind::Identifier) {
      advance();
    } else {
      expect(TokenKind::IntLiteral, "array size");
    }
    expect(TokenKind::RBracket, "']' after array type");
    return ast_.create<StaticArrayType>(
      elem, ast_.intern(size_tok.text), bounded, join_ranges(t.range, size_tok.range));
  }

  if (t.kind == TokenKind::Identifier) {
    advance();

    // bounded string: string<N>
    if (t.text == "string" && match(TokenKind::Lt)) {
      const Token & size_tok = cur();
      expect(TokenKind::IntLiteral, "string bound");
      expect(TokenKind::Gt, "'>' after string bound");
      return ast_.create<PrimaryType>(
        ast_.intern(t.text), ast_.intern(size_tok.text), join_ranges(t.range, size_tok.range));
    }

    return ast_.create<PrimaryType>(ast_.intern(t.text), t.range);
  }

  error_at(t, "expected type");
  advance();
  return ast_.create<PrimaryType>(ast_.intern(""), t.range);
}

Expr * Parser::parse_expr() { return parse_or(); }

Expr * Parser::parse_or()
{
  Expr * lhs = parse_and();
  while (match(TokenKind::OrOr)) {
    Expr * rhs = parse_and();
    lhs = ast_.create<BinaryExpr>(
      lhs, BinaryOp::Or, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_and()
{
  Expr * lhs = parse_bitor();
  while (match(TokenKind::AndAnd)) {
    Expr * rhs = parse_bitor();
    lhs = ast_.create<BinaryExpr>(
      lhs, BinaryOp::And, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_bitor()
{
  Expr * lhs = parse_bitxor();
  while (match(TokenKind::Pipe)) {
    Expr * rhs = parse_bitxor();
    lhs = ast_.create<BinaryExpr>(
      lhs, BinaryOp::BitOr, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_bitxor()
{
  Expr * lhs = parse_bitand();
  while (match(TokenKind::Caret)) {
    Expr * rhs = parse_bitand();
    lhs = ast_.create<BinaryExpr>(
      lhs, BinaryOp::BitXor, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_bitand()
{
  Expr * lhs = parse_equality();
  while (match(TokenKind::Amp)) {
    Expr * rhs = parse_equality();
    lhs = ast_.create<BinaryExpr>(
      lhs, BinaryOp::BitAnd, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_equality()
{
  Expr * lhs = parse_comparison();
  if (match(TokenKind::EqEq) || match(TokenKind::Ne)) {
    const Token op_tok = tokens_[idx_ - 1];
    const BinaryOp op = (op_tok.kind == TokenKind::EqEq) ? BinaryOp::Eq : BinaryOp::Ne;
    Expr * rhs = parse_comparison();
    Expr * out =
      ast_.create<BinaryExpr>(lhs, op, rhs, join_ranges(lhs->get_range(), rhs->get_range()));

    // Reject chaining: a == b == c
    if (cur().kind == TokenKind::EqEq || cur().kind == TokenKind::Ne) {
      error_at(cur(), "chained equality operators are not allowed");
    }
    return out;
  }
  return lhs;
}

Expr * Parser::parse_comparison()
{
  Expr * lhs = parse_add();
  if (
    match(TokenKind::Lt) || match(TokenKind::Le) || match(TokenKind::Gt) || match(TokenKind::Ge)) {
    const Token op_tok = tokens_[idx_ - 1];
    BinaryOp op = BinaryOp::Lt;
    switch (op_tok.kind) {
      case TokenKind::Lt:
        op = BinaryOp::Lt;
        break;
      case TokenKind::Le:
        op = BinaryOp::Le;
        break;
      case TokenKind::Gt:
        op = BinaryOp::Gt;
        break;
      case TokenKind::Ge:
        op = BinaryOp::Ge;
        break;
      default:
        break;
    }

    Expr * rhs = parse_add();
    Expr * out =
      ast_.create<BinaryExpr>(lhs, op, rhs, join_ranges(lhs->get_range(), rhs->get_range()));

    // Reject chaining: a < b < c
    if (
      cur().kind == TokenKind::Lt || cur().kind == TokenKind::Le || cur().kind == TokenKind::Gt ||
      cur().kind == TokenKind::Ge) {
      error_at(cur(), "chained comparison operators are not allowed");
    }

    return out;
  }
  return lhs;
}

Expr * Parser::parse_add()
{
  Expr * lhs = parse_mul();
  while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
    const Token op_tok = tokens_[idx_ - 1];
    const BinaryOp op = (op_tok.kind == TokenKind::Plus) ? BinaryOp::Add : BinaryOp::Sub;
    Expr * rhs = parse_mul();
    lhs = ast_.create<BinaryExpr>(lhs, op, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_mul()
{
  Expr * lhs = parse_unary();
  while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent)) {
    const Token op_tok = tokens_[idx_ - 1];
    BinaryOp op = BinaryOp::Mul;
    if (op_tok.kind == TokenKind::Star) {
      op = BinaryOp::Mul;
    } else if (op_tok.kind == TokenKind::Slash) {
      op = BinaryOp::Div;
    } else {
      op = BinaryOp::Mod;
    }
    Expr * rhs = parse_unary();
    lhs = ast_.create<BinaryExpr>(lhs, op, rhs, join_ranges(lhs->get_range(), rhs->get_range()));
  }
  return lhs;
}

Expr * Parser::parse_unary()
{
  if (match(TokenKind::Bang)) {
    const Token op = tokens_[idx_ - 1];
    Expr * e = parse_unary();
    return ast_.create<UnaryExpr>(UnaryOp::Not, e, join_ranges(op.range, e->get_range()));
  }
  if (match(TokenKind::Minus)) {
    const Token op = tokens_[idx_ - 1];
    Expr * e = parse_unary();
    return ast_.create<UnaryExpr>(UnaryOp::Neg, e, join_ranges(op.range, e->get_range()));
  }
  return parse_postfix();
}

Expr * Parser::parse_postfix()
{
  Expr * e = parse_primary();

  // index
  while (match(TokenKind::LBracket)) {
    Expr * idx = parse_expr();
    const Token & rb = cur();
    expect(TokenKind::RBracket, "']' after index expr");
    e = ast_.create<IndexExpr>(e, idx, join_ranges(e->get_range(), rb.range));
  }

  // cast: expr as type
  while (cur().kind == TokenKind::Identifier && cur().text == "as") {
    const Token as_tok = advance();
    TypeExpr * ty = parse_type_expr();
    e = ast_.create<CastExpr>(e, ty, join_ranges(e->get_range(), ty->get_range()));
    (void)as_tok;
  }

  return e;
}

Expr * Parser::parse_primary()
{
  const Token & t = cur();

  if (match(TokenKind::IntLiteral)) {
    int64_t v = 0;
    (void)std::from_chars(t.text.data(), t.text.data() + t.text.size(), v);
    return ast_.create<IntLiteralExpr>(v, t.range);
  }

  if (match(TokenKind::FloatLiteral)) {
    const std::string tmp(t.text);
    const double v = std::strtod(tmp.c_str(), nullptr);
    return ast_.create<FloatLiteralExpr>(v, t.range);
  }

  if (match(TokenKind::StringLiteral)) {
    const std::string s = unescape_string(t.text, t);
    return ast_.create<StringLiteralExpr>(ast_.intern(s), t.range);
  }

  if (t.kind == TokenKind::Identifier && t.text == "true") {
    advance();
    return ast_.create<BoolLiteralExpr>(true, t.range);
  }
  if (t.kind == TokenKind::Identifier && t.text == "false") {
    advance();
    return ast_.create<BoolLiteralExpr>(false, t.range);
  }
  if (t.kind == TokenKind::Identifier && t.text == "null") {
    advance();
    return ast_.create<NullLiteralExpr>(t.range);
  }

  // vec![...]
  if (
    t.kind == TokenKind::Identifier && t.text == "vec" && cur(1).kind == TokenKind::Bang &&
    cur(2).kind == TokenKind::LBracket) {
    const Token start_tok = advance();
    expect(TokenKind::Bang, "'!' after vec");
    expect(TokenKind::LBracket, "'[' after vec!");
    const Token lb = tokens_[idx_ - 1];

    // Parse array literal/repeat inside
    Expr * first = nullptr;
    std::vector<Expr *> elems;

    if (!at(TokenKind::RBracket)) {
      first = parse_expr();
      if (match(TokenKind::Semicolon)) {
        Expr * cnt = parse_expr();
        const Token & rb = cur();
        expect(TokenKind::RBracket, "']' after array repeat");
        Expr * inner = ast_.create<ArrayRepeatExpr>(first, cnt, join_ranges(lb.range, rb.range));
        return ast_.create<VecMacroExpr>(inner, join_ranges(start_tok.range, rb.range));
      }
      elems.push_back(first);
      while (match(TokenKind::Comma)) {
        if (at(TokenKind::RBracket)) break;
        elems.push_back(parse_expr());
      }
    }

    const Token & rb = cur();
    expect(TokenKind::RBracket, "']' after array literal");

    auto span = ast_.copy_to_arena(elems);
    auto * arr = ast_.create<ArrayLiteralExpr>(span, join_ranges(lb.range, rb.range));
    return ast_.create<VecMacroExpr>(arr, join_ranges(start_tok.range, rb.range));
  }

  // Array literal / repeat
  if (match(TokenKind::LBracket)) {
    const Token lb = tokens_[idx_ - 1];

    std::vector<Expr *> elems;
    if (!at(TokenKind::RBracket)) {
      Expr * first = parse_expr();
      if (match(TokenKind::Semicolon)) {
        Expr * cnt = parse_expr();
        const Token & rb = cur();
        expect(TokenKind::RBracket, "']' after array repeat");
        return ast_.create<ArrayRepeatExpr>(first, cnt, join_ranges(lb.range, rb.range));
      }
      elems.push_back(first);
      while (match(TokenKind::Comma)) {
        if (at(TokenKind::RBracket)) break;
        elems.push_back(parse_expr());
      }
    }

    const Token & rb = cur();
    expect(TokenKind::RBracket, "']' after array literal");

    auto span = ast_.copy_to_arena(elems);
    return ast_.create<ArrayLiteralExpr>(span, join_ranges(lb.range, rb.range));
  }

  if (match(TokenKind::LParen)) {
    Expr * e = parse_expr();
    expect(TokenKind::RParen, "')' after expression");
    return e;
  }

  if (t.kind == TokenKind::Identifier) {
    advance();
    return ast_.create<VarRefExpr>(ast_.intern(t.text), t.range);
  }

  error_at(t, "expected expression");

  // Improved recovery: if we see a likely synchronization token,
  // do NOT consume it. This prevents cascading errors where the
  // statement terminator (;) or block closer (}) is eaten by the expression parser.
  if (
    t.kind != TokenKind::Semicolon && t.kind != TokenKind::RBrace && t.kind != TokenKind::RParen) {
    advance();
  }

  return ast_.create<MissingExpr>(t.range);
}

Expr * Parser::make_missing_expr_at(const Token & t) { return ast_.create<MissingExpr>(t.range); }

std::string Parser::unescape_string(std::string_view raw, const Token & tok_for_diag)
{
  std::string out;
  out.reserve(raw.size());

  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }

    if (i + 1 >= raw.size()) {
      error_at(tok_for_diag, "unterminated escape sequence");
      break;
    }

    const char esc = raw[++i];
    switch (esc) {
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case '0':
        out.push_back('\0');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '"':
        out.push_back('"');
        break;
      case 'u': {
        // \u{HEX}
        bool valid = true;
        if (i + 1 >= raw.size() || raw[i + 1] != '{') {
          error_at(tok_for_diag, "expected \\u{...} escape");
          break;
        }
        i += 1;  // consume '{'
        uint32_t cp = 0;
        bool any = false;
        while (i + 1 < raw.size() && raw[i + 1] != '}') {
          const char h = raw[i + 1];
          uint32_t v = 0;
          if (h >= '0' && h <= '9') {
            v = static_cast<uint32_t>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            v = static_cast<uint32_t>(10 + (h - 'a'));
          } else if (h >= 'A' && h <= 'F') {
            v = static_cast<uint32_t>(10 + (h - 'A'));
          } else {
            error_at(tok_for_diag, "invalid hex digit in \\u{...} escape");
            valid = false;
            break;
          }
          any = true;
          cp = (cp << 4) | v;
          i += 1;
          if (cp > 0x10FFFF) {
            error_at(tok_for_diag, "unicode escape out of range");
            valid = false;
            break;
          }
        }
        if (!any) {
          error_at(tok_for_diag, "empty unicode escape");
          valid = false;
        }
        if (i + 1 < raw.size() && raw[i + 1] == '}') {
          i += 1;  // consume '}'
        } else {
          error_at(tok_for_diag, "unterminated unicode escape");
          valid = false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
          error_at(tok_for_diag, "unicode surrogate not allowed");
          valid = false;
        }
        // Only append the codepoint if the entire escape sequence was valid.
        if (valid) {
          append_utf8(out, cp);
        }
        break;
      }
      default:
        error_at(tok_for_diag, "unknown escape sequence");
        out.push_back(esc);
        break;
    }
  }

  return out;
}

}  // namespace bt_dsl::syntax
