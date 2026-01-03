// bt_dsl/sema/symbol_table.cpp - Symbol table implementation (new AST)
//
#include "bt_dsl/sema/resolution/symbol_table.hpp"

namespace bt_dsl
{

Scope * SymbolTable::get_tree_scope(std::string_view tree_name)
{
  auto it = tree_scopes_.find(tree_name);  // Heterogeneous lookup
  return it != tree_scopes_.end() ? it->second.get() : nullptr;
}

const Scope * SymbolTable::get_tree_scope(std::string_view tree_name) const
{
  auto it = tree_scopes_.find(tree_name);  // Heterogeneous lookup
  return it != tree_scopes_.end() ? it->second.get() : nullptr;
}

std::vector<std::string> SymbolTable::get_tree_names() const
{
  std::vector<std::string> names;
  names.reserve(tree_scopes_.size());
  for (const auto & [name, _] : tree_scopes_) {
    names.emplace_back(name);  // Explicit conversion from string_view to string
  }
  return names;
}

void SymbolTable::build_from_program(const Program & program)
{
  // Clear existing data
  global_scope_ = std::make_unique<Scope>();
  tree_scopes_.clear();

  // Register global variables
  for (const auto * var : program.globalVars) {
    Symbol sym;
    sym.name = var->name;
    sym.kind = SymbolKind::GlobalVariable;
    if (var->type) {
      // Would need to resolve type name from TypeExpr
    }
    sym.direction = std::nullopt;
    sym.definitionRange = var->get_range();
    sym.astNode = var;
    global_scope_->define(sym);
  }

  // Register global constants
  for (const auto * c : program.globalConsts) {
    Symbol sym;
    sym.name = c->name;
    sym.kind = SymbolKind::GlobalConst;
    sym.direction = std::nullopt;
    sym.definitionRange = c->get_range();
    sym.astNode = c;
    global_scope_->define(sym);
  }

  // Build per-tree scopes
  for (const auto * tree : program.trees) {
    build_tree_scope(*tree);
  }
}

void SymbolTable::build_tree_scope(const TreeDecl & tree)
{
  auto scope = std::make_unique<Scope>(global_scope_.get());

  // Add parameters
  for (const auto * param : tree.params) {
    Symbol sym;
    sym.name = param->name;
    sym.kind = SymbolKind::Parameter;
    sym.direction = param->direction;
    sym.definitionRange = param->get_range();
    sym.astNode = param;
    scope->define(sym);
  }

  tree_scopes_.emplace(tree.name, std::move(scope));  // tree.name is already string_view
}

}  // namespace bt_dsl
