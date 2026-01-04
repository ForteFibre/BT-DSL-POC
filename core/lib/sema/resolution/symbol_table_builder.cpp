// bt_dsl/sema/symbol_table_builder.cpp - Symbol table construction implementation
//
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"

#include <string>

#include "bt_dsl/basic/casting.hpp"

namespace bt_dsl
{

// ============================================================================
// Constructor
// ============================================================================

SymbolTableBuilder::SymbolTableBuilder(
  SymbolTable & values, TypeTable & types, NodeRegistry & nodes, DiagnosticBag * diags)
: values_(values), types_(types), nodes_(nodes), diags_(diags)
{
}

// ============================================================================
// Entry Point
// ============================================================================

bool SymbolTableBuilder::build(Program & program)
{
  has_errors_ = false;
  error_count_ = 0;

  // Ensure the value symbol table has its global + per-tree scopes.
  // Many call sites (tests, CLI) expect this builder to be self-contained.
  values_.build_from_program(program);

  // Register extern types (Type namespace)
  for (const auto * ext_type : program.externTypes) {
    if (!ext_type) continue;

    const TypeSymbol * existing = types_.lookup(ext_type->name);
    if (existing != nullptr) {
      // Idempotent: ok if the existing symbol points at the same declaration.
      if (existing->decl != ext_type) {
        report_error(ext_type->get_range(), "redefinition of type");
      }
      continue;
    }

    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    (void)types_.define(sym);
  }

  // Register type aliases (Type namespace)
  for (const auto * alias : program.typeAliases) {
    if (!alias) continue;

    const TypeSymbol * existing = types_.lookup(alias->name);
    if (existing != nullptr) {
      if (existing->decl != alias) {
        report_error(alias->get_range(), "redefinition of type");
      }
      continue;
    }

    TypeSymbol sym;
    sym.name = alias->name;
    sym.decl = alias;
    sym.is_builtin = false;
    (void)types_.define(sym);
  }

  // Register extern nodes (Node namespace)
  for (const auto * ext : program.externs) {
    if (!ext) continue;

    const NodeSymbol * existing = nodes_.lookup(ext->name);
    if (existing != nullptr) {
      if (existing->decl != ext) {
        report_error(ext->get_range(), "redefinition of node");
      }
      continue;
    }

    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    (void)nodes_.define(sym);
  }

  // Register trees (Node namespace)
  for (const auto * tree : program.trees) {
    if (!tree) continue;

    const NodeSymbol * existing = nodes_.lookup(tree->name);
    if (existing != nullptr) {
      if (existing->decl != tree) {
        report_error(tree->get_range(), "redefinition of node");
      }
      continue;
    }

    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    (void)nodes_.define(sym);
  }

  // Validate global value declarations (Value namespace)
  if (Scope * global = values_.get_global_scope()) {
    for (const auto * v : program.globalVars) {
      if (!v) continue;
      if (const Symbol * existing = global->lookup_local(v->name)) {
        if (existing->astNode != v) {
          report_error(v->get_range(), "redefinition of variable");
        }
      }
    }
    for (const auto * c : program.globalConsts) {
      if (!c) continue;
      if (const Symbol * existing = global->lookup_local(c->name)) {
        if (existing->astNode != c) {
          report_error(c->get_range(), "redefinition of constant");
        }
      }
    }
  }

  // Build tree scopes with block scopes
  for (auto * tree : program.trees) {
    build_tree_scope(tree);
  }

  return !has_errors_;
}

// ============================================================================
// Tree Scope Building
// ============================================================================

void SymbolTableBuilder::build_tree_scope(TreeDecl * tree)
{
  // Get tree scope (already created by SymbolTable::buildFromProgram)
  Scope * tree_scope = values_.get_tree_scope(tree->name);
  if (tree_scope == nullptr) {
    return;
  }

  current_scope_ = tree_scope;

  // Validate parameter redefinitions within the tree scope (Value namespace).
  // SymbolTable::build_from_program pre-populates the first declaration; we
  // need to detect duplicates for reference compliance.
  for (const auto * param : tree->params) {
    if (!param) continue;

    // Spec §4.2.3: Shadowing is forbidden across ancestor scopes.
    // Tree params live in the tree scope (parent is global scope).
    check_shadowing(param->name, param->get_range());

    if (const Symbol * existing = current_scope_->lookup_local(param->name)) {
      if (existing->astNode != param) {
        report_error(param->get_range(), "redefinition of parameter");
      }
    }
  }

  // Process body statements to create block scopes
  for (auto * stmt : tree->body) {
    visit(stmt);
  }

  current_scope_ = nullptr;
}

// ============================================================================
// Visitor Methods - Statements
// ============================================================================

void SymbolTableBuilder::visit_node_stmt(NodeStmt * node)
{
  // If this node has a children block, create a block scope
  if (node->hasChildrenBlock && !node->children.empty()) {
    Scope * block_scope = values_.create_block_scope(current_scope_);
    node->resolvedBlockScope = block_scope;

    // Save current scope and enter block scope
    Scope * saved_scope = current_scope_;
    current_scope_ = block_scope;

    // Process children
    for (auto * child : node->children) {
      visit(child);
    }

    // Restore scope
    current_scope_ = saved_scope;
  } else {
    // No children block, just process children normally
    for (auto * child : node->children) {
      visit(child);
    }
  }

  // Process arguments for inline declarations
  for (auto * arg : node->args) {
    visit_argument(arg);
  }
}

void SymbolTableBuilder::visit_blackboard_decl_stmt(BlackboardDeclStmt * node)
{
  // Check for shadowing
  check_shadowing(node->name, node->get_range());

  // Register in current scope
  Symbol sym;
  sym.name = node->name;
  sym.kind = SymbolKind::LocalVariable;
  sym.definitionRange = node->get_range();
  sym.astNode = node;

  if (current_scope_ && !current_scope_->define(sym)) {
    report_error(node->get_range(), "redefinition of variable");
  }
}

void SymbolTableBuilder::visit_const_decl_stmt(ConstDeclStmt * node)
{
  // Check for shadowing
  check_shadowing(node->name, node->get_range());

  // Register in current scope
  Symbol sym;
  sym.name = node->name;
  sym.kind = SymbolKind::LocalConst;
  sym.definitionRange = node->get_range();
  sym.astNode = node;

  if (current_scope_ && !current_scope_->define(sym)) {
    report_error(node->get_range(), "redefinition of constant");
  }
}

void SymbolTableBuilder::visit_assignment_stmt(AssignmentStmt * /* node */)
{
  // Assignments don't define new symbols
}

// ============================================================================
// Visitor Methods - Supporting Nodes
// ============================================================================

void SymbolTableBuilder::visit_argument(Argument * node)
{
  // Register inline blackboard declaration
  if (node->inlineDecl != nullptr) {
    check_shadowing(node->inlineDecl->name, node->inlineDecl->get_range());

    Symbol sym;
    sym.name = node->inlineDecl->name;
    sym.kind = SymbolKind::BlockVariable;
    sym.definitionRange = node->inlineDecl->get_range();
    sym.astNode = node->inlineDecl;

    if (current_scope_ && !current_scope_->define(sym)) {
      report_error(node->inlineDecl->get_range(), "redefinition of variable");
    }
  }
}

// ============================================================================
// Internal Helpers
// ============================================================================

bool SymbolTableBuilder::check_shadowing(std::string_view name, SourceRange range)
{
  // Spec §4.2.3: Shadowing is forbidden
  // Check if name exists in any parent scope
  if (current_scope_ != nullptr) {
    const Scope * parent = current_scope_->get_parent();
    while (parent != nullptr) {
      if (parent->lookup_local(name) != nullptr) {
        report_error(range, "declaration shadows previous declaration");
        return true;
      }
      parent = parent->get_parent();
    }
  }
  return false;
}

void SymbolTableBuilder::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  ++error_count_;

  if (diags_ != nullptr) {
    diags_->error(range, message);
  }
}

}  // namespace bt_dsl
