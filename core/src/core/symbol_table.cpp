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
         kind == SymbolKind::Parameter;
}

bool Symbol::is_writable() const
{
  if (!direction) {
    return false;
  }
  return *direction == PortDirection::Out || *direction == PortDirection::Ref;
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

  // Register global variables
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

  // Register trees as symbols (for cross-tree references)
  for (const auto & tree : program.trees) {
    Symbol sym;
    sym.name = tree.name;
    sym.kind = SymbolKind::Tree;
    sym.type_name = std::nullopt;
    sym.direction = std::nullopt;
    sym.definition_range = tree.range;
    sym.ast_node = &tree;
    global_scope_->define(std::move(sym));

    // Build tree-local scope
    build_tree_scope(tree);
  }

  // Register declared nodes as symbols
  for (const auto & decl : program.declarations) {
    Symbol sym;
    sym.name = decl.name;
    sym.kind = SymbolKind::DeclaredNode;
    sym.type_name = std::nullopt;
    sym.direction = std::nullopt;
    sym.definition_range = decl.range;
    sym.ast_node = &decl;
    global_scope_->define(std::move(sym));
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

  // Add local variables
  for (const auto & local : tree.local_vars) {
    Symbol sym;
    sym.name = local.name;
    sym.kind = SymbolKind::LocalVariable;
    sym.type_name = local.type_name;
    sym.direction = std::nullopt;
    sym.definition_range = local.range;
    sym.ast_node = &local;
    scope->define(std::move(sym));
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

}  // namespace bt_dsl
