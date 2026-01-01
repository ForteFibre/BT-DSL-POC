// bt_dsl/symbol_table.cpp - Symbol table implementation
#include "bt_dsl/core/symbol_table.hpp"

namespace bt_dsl
{

// ============================================================================
// Symbol Implementation
// ============================================================================

bool Symbol::is_variable() const
{
  return kind == SymbolKind::GlobalVariable || kind == SymbolKind::LocalVariable ||
         kind == SymbolKind::BlockVariable || kind == SymbolKind::Parameter;
}

bool Symbol::is_const() const
{
  return kind == SymbolKind::GlobalConst || kind == SymbolKind::LocalConst ||
         kind == SymbolKind::BlockConst;
}

bool Symbol::is_writable() const
{
  if (!direction) {
    return false;
  }
  // Reference spec:
  // - in/ref parameters are input-only
  // - mut/out parameters allow writes
  return *direction == PortDirection::Out || *direction == PortDirection::Mut;
}

// ============================================================================
// Scope Implementation
// ============================================================================

Scope::Scope(Scope * parent) : parent_(parent) {}

bool Scope::define(Symbol symbol)
{
  auto [it, inserted] = symbols_.emplace(symbol.name, std::move(symbol));
  return inserted;
}

void Scope::upsert(Symbol symbol) { symbols_.insert_or_assign(symbol.name, std::move(symbol)); }

const Symbol * Scope::lookup_local(std::string_view name) const
{
  const std::string key(name);
  auto it = symbols_.find(key);
  if (it != symbols_.end()) {
    return &it->second;
  }
  return nullptr;
}

const Symbol * Scope::lookup(std::string_view name) const
{
  // First, check this scope
  if (const Symbol * sym = lookup_local(name)) {
    return sym;
  }
  // Then, check parent scope
  if (parent_) {
    return parent_->lookup(name);
  }
  return nullptr;
}

bool Scope::contains(std::string_view name) const { return lookup_local(name) != nullptr; }

// ============================================================================
// SymbolTable Implementation
// ============================================================================

SymbolTable::SymbolTable() : global_scope_(std::make_unique<Scope>()) {}

void SymbolTable::build_from_program(const Program & program)
{
  // Clear existing data
  global_scope_ = std::make_unique<Scope>();
  tree_scopes_.clear();

  // Register global variables (var)
  for (const auto & var : program.global_vars) {
    Symbol sym;
    sym.name = var.name;
    sym.kind = SymbolKind::GlobalVariable;
    sym.type_name = var.type_name;
    sym.direction = std::nullopt;
    sym.definition_range = var.range;
    sym.ast_node = &var;
    global_scope_->define(std::move(sym));
  }

  // Register global constants (const)
  for (const auto & c : program.global_consts) {
    Symbol sym;
    sym.name = c.name;
    sym.kind = SymbolKind::GlobalConst;
    sym.type_name = c.type_name;
    sym.direction = std::nullopt;
    sym.definition_range = c.range;
    sym.ast_node = &c;
    global_scope_->define(std::move(sym));
  }

  // Build per-tree scopes (parameters + tree-local var/const).
  // Note: Node-space identifiers (tree names, extern node names) are handled
  // by NodeRegistry, not by SymbolTable (reference: declarations-and-scopes.md 4.1.1).
  for (const auto & tree : program.trees) {
    build_tree_scope(tree);
  }
}

void SymbolTable::build_tree_scope(const TreeDef & tree)
{
  // Create scope with global as parent
  auto scope = std::make_unique<Scope>(global_scope_.get());

  // Add parameters
  for (const auto & param : tree.params) {
    Symbol sym;
    sym.name = param.name;
    sym.kind = SymbolKind::Parameter;
    sym.type_name = param.type_name;
    sym.direction = param.direction;
    sym.definition_range = param.range;
    sym.ast_node = &param;
    scope->define(std::move(sym));
  }

  // Add top-level local vars/consts declared in the tree body.
  // NOTE: vars/consts inside nested children blocks are block-scoped and are
  // introduced during semantic validation (Analyzer::validate_statement_block).
  for (const auto & stmt : tree.body) {
    std::visit(
      [&](const auto & s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
          Symbol sym;
          sym.name = s.name;
          sym.kind = SymbolKind::LocalVariable;
          sym.type_name = s.type_name;
          sym.direction = std::nullopt;
          sym.definition_range = s.range;
          sym.ast_node = &s;
          scope->define(std::move(sym));
        } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
          Symbol sym;
          sym.name = s.name;
          sym.kind = SymbolKind::LocalConst;
          sym.type_name = s.type_name;
          sym.direction = std::nullopt;
          sym.definition_range = s.range;
          sym.ast_node = &s;
          scope->define(std::move(sym));
        }
      },
      stmt);
  }

  tree_scopes_.emplace(tree.name, std::move(scope));
}

Scope * SymbolTable::tree_scope(std::string_view tree_name)
{
  const std::string key(tree_name);
  auto it = tree_scopes_.find(key);
  if (it != tree_scopes_.end()) {
    return it->second.get();
  }
  return nullptr;
}

const Scope * SymbolTable::tree_scope(std::string_view tree_name) const
{
  const std::string key(tree_name);
  auto it = tree_scopes_.find(key);
  if (it != tree_scopes_.end()) {
    return it->second.get();
  }
  return nullptr;
}

const Symbol * SymbolTable::resolve(std::string_view name, const Scope * from_scope) const
{
  if (from_scope) {
    return from_scope->lookup(name);
  }
  return global_scope_->lookup(name);
}

std::vector<std::string> SymbolTable::tree_names() const
{
  std::vector<std::string> names;
  names.reserve(tree_scopes_.size());
  for (const auto & [name, _] : tree_scopes_) {
    names.push_back(name);
  }
  return names;
}

bool SymbolTable::has_global(std::string_view name) const { return global_scope_->contains(name); }

const Symbol * SymbolTable::get_global(std::string_view name) const
{
  return global_scope_->lookup_local(name);
}

bool SymbolTable::try_define_global(Symbol symbol)
{
  return global_scope_->define(std::move(symbol));
}

void SymbolTable::upsert_global(Symbol symbol) { global_scope_->upsert(std::move(symbol)); }

}  // namespace bt_dsl
