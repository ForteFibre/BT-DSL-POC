// bt_dsl/sema/name_resolver.cpp - Name resolution implementation
//
#include "bt_dsl/sema/resolution/name_resolver.hpp"

#include "bt_dsl/basic/casting.hpp"

namespace bt_dsl
{

// ============================================================================
// Entry Point
// ============================================================================

bool NameResolver::resolve()
{
  has_errors_ = false;
  error_count_ = 0;

  if (!module_.program) {
    return true;
  }

  const Program & program = *module_.program;

  // Start from global scope
  current_scope_ = module_.values.get_global_scope();

  // Resolve global var initializers
  for (auto * var : program.globalVars) {
    if (var->type) {
      visit(var->type);
    }
    if (var->initialValue) {
      visit(var->initialValue);
    }
  }

  // Resolve global const values
  for (auto * c : program.globalConsts) {
    if (c->type) {
      visit(c->type);
    }
    if (c->value) {
      visit(c->value);
    }
  }

  // Resolve type alias definitions
  for (auto * alias : program.typeAliases) {
    if (alias->aliasedType) {
      visit(alias->aliasedType);
    }
  }

  // Resolve extern port types
  for (auto * ext : program.externs) {
    for (auto * port : ext->ports) {
      visit_extern_port(port);
    }
  }

  // Resolve tree definitions
  for (auto * tree : program.trees) {
    visit_tree_decl(tree);
  }

  return !has_errors_;
}

// ============================================================================
// Cross-Module Lookup
// ============================================================================

const TypeSymbol * NameResolver::lookup_type(std::string_view name, SourceRange range)
{
  // 1. Local lookup first
  if (const TypeSymbol * sym = module_.types.lookup(name)) {
    return sym;
  }

  // 2. Collect from imports (public only)
  std::vector<const TypeSymbol *> candidates;
  for (auto * imported : module_.imports) {
    if (const TypeSymbol * sym = imported->types.lookup(name)) {
      if (ModuleInfo::is_public(name)) {
        candidates.push_back(sym);
      }
    }
  }

  // 3. Check ambiguity
  if (candidates.size() > 1) {
    report_error(range, "ambiguous reference to type '" + std::string(name) + "'");
    return nullptr;
  }

  if (!candidates.empty()) {
    return candidates[0];
  }

  return nullptr;
}

const NodeSymbol * NameResolver::lookup_node(std::string_view name, SourceRange range)
{
  // 1. Local lookup first
  if (const NodeSymbol * sym = module_.nodes.lookup(name)) {
    return sym;
  }

  // 2. Collect from imports (public only)
  std::vector<const NodeSymbol *> candidates;
  for (auto * imported : module_.imports) {
    if (const NodeSymbol * sym = imported->nodes.lookup(name)) {
      if (ModuleInfo::is_public(name)) {
        candidates.push_back(sym);
      }
    }
  }

  // 3. Check ambiguity
  if (candidates.size() > 1) {
    report_error(range, "ambiguous reference to node '" + std::string(name) + "'");
    return nullptr;
  }

  if (!candidates.empty()) {
    return candidates[0];
  }

  return nullptr;
}

const Symbol * NameResolver::lookup_value(std::string_view name, Scope * scope, SourceRange range)
{
  // 1. Scope chain lookup (local → tree local → global)
  if (const Symbol * sym = module_.values.resolve(name, scope)) {
    return sym;
  }

  // 2. Collect from imports (public only, global scope)
  std::vector<const Symbol *> candidates;
  for (auto * imported : module_.imports) {
    Scope * global_scope = imported->values.get_global_scope();
    if (global_scope) {
      if (const Symbol * sym = global_scope->lookup_local(name)) {
        if (ModuleInfo::is_public(name)) {
          candidates.push_back(sym);
        }
      }
    }
  }

  // 3. Check ambiguity
  if (candidates.size() > 1) {
    report_error(range, "ambiguous reference to '" + std::string(name) + "'");
    return nullptr;
  }

  if (!candidates.empty()) {
    return candidates[0];
  }

  return nullptr;
}

namespace
{

// Spec (§4.2.4): In block scopes (tree bodies), Value-space references must be
// after the declaration. In module scope, only top-level const allows forward
// reference; global vars must not be referenced before declaration.
bool violates_value_forward_reference(const Symbol & sym, SourceRange use_range)
{
  if (!use_range.is_valid() || !sym.definitionRange.is_valid()) {
    return false;
  }

  // Only enforce for Value-space symbols that are required to be declared
  // before use. Top-level const explicitly allows forward reference.
  if (sym.kind == SymbolKind::GlobalConst) {
    return false;
  }

  // For everything else in Value space, enforce declaration-before-use.
  switch (sym.kind) {
    case SymbolKind::GlobalVariable:
    case SymbolKind::LocalVariable:
    case SymbolKind::LocalConst:
    case SymbolKind::BlockVariable:
    case SymbolKind::BlockConst:
    case SymbolKind::Parameter:
      return use_range.get_begin() < sym.definitionRange.get_begin();
    default:
      return false;
  }
}

}  // namespace

// ============================================================================
// Visitor Methods - Expressions
// ============================================================================

void NameResolver::visit_var_ref_expr(VarRefExpr * node)
{
  // First resolve within the current module. If it resolves to a local/global
  // symbol, we can enforce declaration-before-use (§4.2.4) using byte offsets.
  if (const Symbol * sym = module_.values.resolve(node->name, current_scope_)) {
    if (violates_value_forward_reference(*sym, node->get_range())) {
      report_error(node->get_range(), "use of identifier before declaration");
    }
    node->resolvedSymbol = sym;
    return;
  }

  // Otherwise, try imported modules (no forward-reference rule across modules).
  const Symbol * sym = lookup_value(node->name, current_scope_, node->get_range());
  if (!sym) {
    report_error(node->get_range(), "use of undeclared identifier");
    return;
  }
  node->resolvedSymbol = sym;
}

void NameResolver::visit_binary_expr(BinaryExpr * node)
{
  if (node->lhs) visit(node->lhs);
  if (node->rhs) visit(node->rhs);
}

void NameResolver::visit_unary_expr(UnaryExpr * node)
{
  if (node->operand) visit(node->operand);
}

void NameResolver::visit_cast_expr(CastExpr * node)
{
  if (node->expr) visit(node->expr);
  if (node->targetType) visit(node->targetType);
}

void NameResolver::visit_index_expr(IndexExpr * node)
{
  if (node->base) visit(node->base);
  if (node->index) visit(node->index);
}

void NameResolver::visit_array_literal_expr(ArrayLiteralExpr * node)
{
  for (auto * elem : node->elements) {
    if (elem) visit(elem);
  }
}

void NameResolver::visit_array_repeat_expr(ArrayRepeatExpr * node)
{
  if (node->value) visit(node->value);
  if (node->count) visit(node->count);
}

void NameResolver::visit_vec_macro_expr(VecMacroExpr * node)
{
  if (node->inner) visit(node->inner);
}

// ============================================================================
// Visitor Methods - Types
// ============================================================================

void NameResolver::visit_primary_type(PrimaryType * node)
{
  const TypeSymbol * sym = lookup_type(node->name, node->get_range());
  if (!sym) {
    report_error(node->get_range(), "use of undeclared type");
    return;
  }
  node->resolvedType = sym;
}

void NameResolver::visit_static_array_type(StaticArrayType * node)
{
  if (node->elementType) visit(node->elementType);
}

void NameResolver::visit_dynamic_array_type(DynamicArrayType * node)
{
  if (node->elementType) visit(node->elementType);
}

void NameResolver::visit_type_expr(TypeExpr * node)
{
  if (node->base) visit(node->base);
}

// ============================================================================
// Visitor Methods - Statements
// ============================================================================

void NameResolver::visit_node_stmt(NodeStmt * node)
{
  // Resolve node name
  const NodeSymbol * sym = lookup_node(node->nodeName, node->get_range());
  if (!sym) {
    report_error(node->get_range(), "use of undeclared node");
  } else {
    node->resolvedNode = sym;
  }

  // Resolve preconditions
  for (auto * pre : node->preconditions) {
    visit_precondition(pre);
  }

  // Resolve arguments
  for (auto * arg : node->args) {
    visit_argument(arg);
  }

  // Resolve children using pre-built block scope from SymbolTableBuilder
  if (node->hasChildrenBlock && node->resolvedBlockScope != nullptr) {
    push_scope(node->resolvedBlockScope);
    for (auto * child : node->children) {
      visit(child);
    }
    pop_scope();
  } else {
    for (auto * child : node->children) {
      visit(child);
    }
  }
}

void NameResolver::visit_assignment_stmt(AssignmentStmt * node)
{
  // Resolve target
  if (const Symbol * sym = module_.values.resolve(node->target, current_scope_)) {
    if (violates_value_forward_reference(*sym, node->get_range())) {
      report_error(node->get_range(), "use of identifier before declaration");
    }
    node->resolvedTarget = sym;
  } else {
    const Symbol * imported = lookup_value(node->target, current_scope_, node->get_range());
    if (!imported) {
      report_error(node->get_range(), "use of undeclared identifier");
    } else {
      node->resolvedTarget = imported;
    }
  }

  // Resolve indices
  for (auto * idx : node->indices) {
    if (idx) visit(idx);
  }

  // Resolve value
  if (node->value) visit(node->value);

  // Resolve preconditions
  for (auto * pre : node->preconditions) {
    visit_precondition(pre);
  }
}

void NameResolver::visit_blackboard_decl_stmt(BlackboardDeclStmt * node)
{
  // Symbol already registered by SymbolTableBuilder
  // Just resolve type and initializer expressions
  if (node->type) visit(node->type);
  if (node->initialValue) visit(node->initialValue);
}

void NameResolver::visit_const_decl_stmt(ConstDeclStmt * node)
{
  // Symbol already registered by SymbolTableBuilder
  // Just resolve type and value expressions
  if (node->type) visit(node->type);
  if (node->value) visit(node->value);
}

// ============================================================================
// Visitor Methods - Declarations
// ============================================================================

void NameResolver::visit_tree_decl(TreeDecl * node)
{
  // Get or create tree scope
  Scope * tree_scope = module_.values.get_tree_scope(node->name);
  if (!tree_scope) {
    // This shouldn't happen if buildFromProgram was called, but handle it
    tree_scope = module_.values.get_global_scope();
  }

  push_scope(tree_scope);

  // Register parameters if not already done
  for (auto * param : node->params) {
    visit_param_decl(param);
  }

  // Resolve body statements
  for (auto * stmt : node->body) {
    visit(stmt);
  }

  pop_scope();
}

// ============================================================================
// Visitor Methods - Supporting Nodes
// ============================================================================

void NameResolver::visit_argument(Argument * node)
{
  // Resolve value expression (inline decl symbol registered by SymbolTableBuilder)
  if (node->valueExpr) {
    visit(node->valueExpr);
  }
}

void NameResolver::visit_precondition(Precondition * node)
{
  if (node->condition) {
    visit(node->condition);
  }
}

void NameResolver::visit_param_decl(ParamDecl * node)
{
  // Resolve type
  if (node->type) visit(node->type);

  // Resolve default value
  if (node->defaultValue) visit(node->defaultValue);
}

void NameResolver::visit_extern_port(ExternPort * node)
{
  if (node->type) visit(node->type);
  if (node->defaultValue) visit(node->defaultValue);
}

// ============================================================================
// Internal Helpers
// ============================================================================

void NameResolver::push_scope(Scope * scope) { current_scope_ = scope; }

void NameResolver::pop_scope()
{
  if (current_scope_) {
    current_scope_ = current_scope_->get_parent();
  }
}

Scope * NameResolver::push_block_scope()
{
  auto scope = std::make_unique<Scope>(current_scope_);
  Scope * ptr = scope.get();
  block_scopes_.push_back(std::move(scope));
  current_scope_ = ptr;
  return ptr;
}

void NameResolver::pop_block_scope()
{
  if (!block_scopes_.empty()) {
    current_scope_ = current_scope_->get_parent();
    block_scopes_.pop_back();
  }
}

bool NameResolver::check_shadowing(std::string_view name, SourceRange range)
{
  // Check if name exists in any parent scope
  if (current_scope_) {
    const Scope * parent = current_scope_->get_parent();
    while (parent) {
      if (parent->lookup_local(name)) {
        report_error(range, "declaration shadows previous declaration");
        return true;
      }
      parent = parent->get_parent();
    }
  }
  return false;
}

void NameResolver::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  error_count_++;

  if (diags_) {
    diags_->error(range, message);
  }
}

}  // namespace bt_dsl
