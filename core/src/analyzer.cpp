// bt_dsl/analyzer.cpp - Semantic analyzer implementation
#include "bt_dsl/analyzer.hpp"

#include <algorithm>
#include <unordered_set>

namespace bt_dsl
{

namespace
{
// Thread-local pointer to the Program currently being validated.
// validate_tree sets this before walking nodes.
thread_local const bt_dsl::Program * g_current_program_for_validation = nullptr;
}  // namespace

// ============================================================================
// AnalysisResult Implementation
// ============================================================================

const TypeContext * AnalysisResult::get_tree_context(std::string_view tree_name) const
{
  const std::string key(tree_name);
  auto it = tree_type_contexts.find(key);
  if (it != tree_type_contexts.end()) {
    return &it->second;
  }
  return nullptr;
}

// ============================================================================
// Analyzer Implementation
// ============================================================================

Analyzer::Analyzer() = default;

AnalysisResult Analyzer::analyze(const Program & program) { return analyze(program, {}); }

AnalysisResult Analyzer::analyze(
  const Program & program, const std::vector<const Program *> & imported_programs)
{
  AnalysisResult result;

  // First, collect declarations from imported programs
  // NOTE: NodeRegistry::build_from_program() clears the registry, so we must
  // merge per-program to preserve declarations across multiple imports.
  for (const auto * imported : imported_programs) {
    if (!imported) {
      continue;
    }
    NodeRegistry imported_nodes;
    imported_nodes.build_from_program(*imported);
    result.nodes.merge(imported_nodes);
  }

  // Then collect from main program (may override imports)
  collect_declarations(program, result);

  // Check for duplicates
  check_duplicates(program, result);

  // Resolve types
  resolve_types(program, result);

  // Validate semantics
  validate_semantics(program, result);

  return result;
}

// ============================================================================
// Declaration Collection
// ============================================================================

void Analyzer::collect_declarations(const Program & program, AnalysisResult & result)
{
  result.symbols.build_from_program(program);

  // Merge with imported nodes (already in result.nodes)
  NodeRegistry program_nodes;
  program_nodes.build_from_program(program);
  result.nodes.merge(program_nodes);
}

// ============================================================================
// Duplicate Checking
// ============================================================================

void Analyzer::check_duplicates(const Program & program, AnalysisResult & result)
{
  check_duplicate_trees(program, result);
  check_duplicate_globals(program, result);
  check_duplicate_declares(program, result);
  check_declare_conflicts(program, result);

  // Check per-tree duplicates
  for (const auto & tree : program.trees) {
    check_duplicate_params(tree, result);
  }
}

void Analyzer::check_duplicate_trees(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const TreeDef *> seen;

  for (const auto & tree : program.trees) {
    auto [it, inserted] = seen.emplace(tree.name, &tree);
    if (!inserted) {
      result.diagnostics.error(tree.range, "Duplicate tree name: '" + tree.name + "'");
    }
  }
}

void Analyzer::check_duplicate_globals(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const GlobalVarDecl *> seen;

  for (const auto & var : program.global_vars) {
    auto [it, inserted] = seen.emplace(var.name, &var);
    if (!inserted) {
      result.diagnostics.error(var.range, "Duplicate global variable name: '" + var.name + "'");
    }
  }
}

void Analyzer::check_duplicate_params(const TreeDef & tree, AnalysisResult & result)
{
  std::unordered_set<std::string> seen;

  for (const auto & param : tree.params) {
    auto [it, inserted] = seen.insert(param.name);
    if (!inserted) {
      result.diagnostics.error(param.range, "Duplicate parameter name: '" + param.name + "'");
    }
  }
}

void Analyzer::check_duplicate_declares(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const DeclareStmt *> seen;

  for (const auto & decl : program.declarations) {
    auto [it, inserted] = seen.emplace(decl.name, &decl);
    if (!inserted) {
      result.diagnostics.error(decl.range, "Duplicate declaration name: '" + decl.name + "'");
    }
  }
}

void Analyzer::check_declare_conflicts(const Program & program, AnalysisResult & result)
{
  std::unordered_set<std::string> tree_names;
  for (const auto & tree : program.trees) {
    tree_names.insert(tree.name);
  }

  for (const auto & decl : program.declarations) {
    if (tree_names.count(decl.name)) {
      result.diagnostics.error(
        decl.range, "Declaration '" + decl.name + "' conflicts with a Tree definition");
    }
  }
}

// ============================================================================
// Type Resolution
// ============================================================================

void Analyzer::resolve_types(const Program & program, AnalysisResult & result)
{
  TypeResolver resolver(result.symbols, result.nodes);

  for (const auto & tree : program.trees) {
    TypeContext ctx = resolver.resolve_tree_types(tree);
    result.tree_type_contexts.emplace(tree.name, std::move(ctx));
  }
}

// ============================================================================
// Semantic Validation
// ============================================================================

void Analyzer::validate_semantics(const Program & program, AnalysisResult & result)
{
  // Validate declare statements
  for (const auto & decl : program.declarations) {
    validate_declare_stmt(decl, result);
  }

  // Validate trees
  for (const auto & tree : program.trees) {
    validate_tree(program, tree, result);
  }
}

void Analyzer::validate_tree(const Program & program, const TreeDef & tree, AnalysisResult & result)
{
  const TypeContext * ctx = result.get_tree_context(tree.name);
  if (!ctx) {
    return;
  }

  // Validate local variables
  validate_local_vars(tree, *ctx, result);

  // Validate body node (not root_node)
  if (tree.body) {
    g_current_program_for_validation = &program;
    validate_node_stmt(*tree.body, tree, *ctx, result);
    g_current_program_for_validation = nullptr;
  }

  // Check for unused write parameters
  check_write_param_usage(tree, result);
}

void Analyzer::validate_node_stmt(
  const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result)
{
  // Validate node category constraints
  validate_node_category(node, result);

  // Validate decorators
  for (const auto & decorator : node.decorators) {
    validate_decorator(decorator, result);
  }

  // Validate arguments
  validate_arguments(node, tree, ctx, result);

  // Recurse into children (ChildElement = variant<Box<NodeStmt>,
  // AssignmentStmt>)
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          validate_node_stmt(*elem, tree, ctx, result);
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          // Validate assignment statements (type checks + symbol resolution)
          if (g_current_program_for_validation) {
            validate_assignment_stmt(*g_current_program_for_validation, elem, tree, ctx, result);
          }
        }
      },
      child);
  }
}

void Analyzer::validate_assignment_stmt(
  const Program & program, const AssignmentStmt & stmt, const TreeDef & tree,
  const TypeContext & ctx, AnalysisResult & result)
{
  const Scope * scope = result.symbols.tree_scope(tree.name);
  const Symbol * sym = result.symbols.resolve(stmt.target, scope);

  if (!sym || !sym->is_variable()) {
    result.diagnostics.error(stmt.range, "Unknown variable: " + stmt.target);
    return;
  }

  // Assignment to an input-only parameter is not allowed.
  if (sym->kind == SymbolKind::Parameter && !sym->is_writable()) {
    result.diagnostics.error(
      stmt.range, "Parameter '" + sym->name + "' is input-only and cannot be assigned");
    return;
  }

  // Build global type cache so TypeResolver can return stable pointers.
  std::unordered_map<std::string, Type> global_types;
  global_types.reserve(program.global_vars.size());
  for (const auto & gv : program.global_vars) {
    global_types.emplace(gv.name, Type::from_string(gv.type_name));
  }

  auto get_global_type = [&](std::string_view name) -> const Type * {
    auto it = global_types.find(std::string(name));
    if (it == global_types.end()) {
      return nullptr;
    }
    return &it->second;
  };

  // Determine target type
  Type target_type = Type::unknown();
  if (sym->type_name) {
    target_type = Type::from_string(*sym->type_name);
  } else if (const Type * t = ctx.get_type(sym->name)) {
    target_type = *t;
  }

  const TypeResolver resolver(result.symbols, result.nodes);
  auto rhs_result = resolver.infer_expression_type(stmt.value, ctx, get_global_type);
  if (rhs_result.has_error()) {
    result.diagnostics.error(stmt.range, rhs_result.error.value_or("Unknown error"));
    return;
  }

  const Type & rhs_type = rhs_result.type;

  auto emit_assign_error = [&](std::string_view msg) {
    result.diagnostics.error(stmt.range, std::string(msg));
  };

  if (stmt.op == AssignOp::Assign) {
    if (!target_type.is_compatible_with(rhs_type)) {
      emit_assign_error("Cannot assign " + rhs_type.to_string() + " to " + target_type.to_string());
    }
    return;
  }

  // Compound assignments: treat as target = target <op> rhs
  // Minimal semantics: require numeric for -=, *=, /=; allow numeric or (string
  // + string) for +=
  if (stmt.op == AssignOp::AddAssign) {
    const bool numeric_ok = target_type.is_numeric() && rhs_type.is_numeric();
    const bool string_ok =
      target_type.equals(Type::string_type()) && rhs_type.equals(Type::string_type());
    if (!(numeric_ok || string_ok)) {
      emit_assign_error("Operator cannot be applied to non-numeric types");
    }
    return;
  }

  // -=, *=, /=
  if (!(target_type.is_numeric() && rhs_type.is_numeric())) {
    emit_assign_error("Operator cannot be applied to non-numeric types");
  }
}

void Analyzer::validate_node_category(const NodeStmt & node, AnalysisResult & result)
{
  const NodeInfo * info = result.nodes.get_node(node.node_name);
  if (!info) {
    result.diagnostics.error(node.range, "Unknown node: '" + node.node_name + "'");
    return;
  }

  // Decorators cannot be used as normal nodes
  if (info->category == NodeCategory::Decorator) {
    result.diagnostics.error(
      node.range, "Decorator node '" + node.node_name + "' cannot be used as a normal node");
    // Continue to collect more errors
  }

  // Check children constraints
  if (node.has_children_block && !info->can_have_children()) {
    result.diagnostics.error(
      node.range, "Node '" + node.node_name + "' cannot have a children block. " +
                    "Only 'Control' category nodes can have children.");
  }

  // Check that Control nodes are used with an explicit children block.
  // (Empty blocks are allowed; semantic arity checks are out of scope here.)
  if (info->category == NodeCategory::Control && !node.has_children_block) {
    result.diagnostics.error(
      node.range, "Control node '" + node.node_name + "' requires a children block");
  }
}

void Analyzer::validate_arguments(
  const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result)
{
  const NodeInfo * info = result.nodes.get_node(node.node_name);
  if (!info) {
    return;
  }

  // Count positional arguments (Argument.name is the port name, nullopt =
  // positional)
  int positional_count = 0;
  for (const auto & arg : node.args) {
    if (!arg.name) {
      positional_count++;
    }
  }

  // Check positional argument constraints
  if (positional_count > 1) {
    result.diagnostics.error(node.range, "Only one positional argument is allowed");
  }

  if (positional_count == 1 && info->port_count() != 1) {
    result.diagnostics.error(
      node.range, "Positional arguments require exactly 1 port, but '" + node.node_name + "' has " +
                    std::to_string(info->port_count()) + " ports");
  }

  // Validate each argument (named or positional)
  const Scope * scope = result.symbols.tree_scope(tree.name);

  for (const auto & arg : node.args) {
    // Determine target port name (positional uses single port)
    std::optional<std::string> port_name = arg.name;
    if (!port_name) {
      port_name = info->get_single_port_name();
    }
    if (!port_name) {
      continue;
    }

    const PortInfo * port = info->get_port(*port_name);
    if (!port) {
      // Unknown port - warn and continue
      result.diagnostics.warning(
        arg.range, "Unknown port '" + *port_name + "' for node '" + node.node_name + "'");
      continue;
    }

    // Only BlackboardRef needs symbol/type/direction checks
    const BlackboardRef * ref = std::get_if<BlackboardRef>(&arg.value);
    if (!ref) {
      continue;
    }

    // Symbol resolution
    const Symbol * var_sym = result.symbols.resolve(ref->name, scope);
    if (!var_sym || !var_sym->is_variable()) {
      result.diagnostics.error(ref->range, "Unknown variable: " + ref->name);
      continue;
    }

    // Type check: var type vs port type
    if (port->type_name) {
      const Type port_type = Type::from_string(*port->type_name);
      Type var_type = Type::unknown();
      if (var_sym->type_name) {
        var_type = Type::from_string(*var_sym->type_name);
      } else if (const Type * t = ctx.get_type(var_sym->name)) {
        var_type = *t;
      }

      if (
        !var_type.is_unknown() && !port_type.is_unknown() &&
        !port_type.is_compatible_with(var_type)) {
        result.diagnostics.error(
          ref->range, "Type mismatch: '" + var_sym->name + "' is '" + var_type.to_string() +
                        "' but port '" + *port_name + "' expects '" + port_type.to_string() + "'.");
      }
    }

    // Direction compatibility (legacy compatibility matrix)
    const PortDirection arg_dir = ref->direction.value_or(PortDirection::In);
    const PortDirection port_dir = port->direction;

    const auto is_write_dir = [](PortDirection d) {
      return d == PortDirection::Out || d == PortDirection::Ref;
    };

    // in -> out/ref is error
    if (arg_dir == PortDirection::In && is_write_dir(port_dir)) {
      result.diagnostics.error(
        ref->range, "Port '" + *port_name + "' requires '" + std::string(to_string(port_dir)) +
                      "' but argument is 'in'. Add '" + std::string(to_string(port_dir)) +
                      "' to enable write access.");
    }

    // out -> in/ref is error
    if (
      arg_dir == PortDirection::Out &&
      (port_dir == PortDirection::In || port_dir == PortDirection::Ref)) {
      result.diagnostics.error(
        ref->range, "Port '" + *port_name + "' is '" + std::string(to_string(port_dir)) +
                      "' but argument is 'out'.");
    }

    // ref -> in/out is warning
    if (arg_dir == PortDirection::Ref && port_dir == PortDirection::In) {
      result.diagnostics.warning(
        ref->range, "Port '" + *port_name + "' is input-only. 'ref' write access will be ignored.");
    } else if (arg_dir == PortDirection::Ref && port_dir == PortDirection::Out) {
      result.diagnostics.warning(
        ref->range,
        "Port '" + *port_name + "' is output-only. Consider using 'out' instead of 'ref'.");
    }

    // Parameter permission: cannot use out/ref on input-only param
    if (var_sym->kind == SymbolKind::Parameter) {
      const PortDirection param_dir = var_sym->direction.value_or(PortDirection::In);
      const bool needs_write = is_write_dir(arg_dir);
      const bool param_allows = is_write_dir(param_dir);
      if (needs_write && !param_allows) {
        result.diagnostics.error(
          ref->range, "Parameter '" + var_sym->name + "' is input-only but used with '" +
                        std::string(to_string(arg_dir)) + "' for write access");
      }
    }
  }
}

void Analyzer::validate_decorator(const Decorator & decorator, AnalysisResult & result)
{
  const NodeInfo * info = result.nodes.get_node(decorator.name);
  if (!info) {
    result.diagnostics.error(decorator.range, "Unknown decorator: '" + decorator.name + "'");
    return;
  }

  if (info->category != NodeCategory::Decorator) {
    result.diagnostics.error(
      decorator.range, "'" + decorator.name + "' is not a Decorator. " +
                         "Only Decorator category nodes can be used as @decorators.");
  }
}

void Analyzer::check_direction_permission(
  const Argument & arg, const PortInfo * port, const TreeDef & tree, const TypeContext & /* ctx */,
  AnalysisResult & result)
{
  // Deprecated by validate_arguments' unified checks; keep for API
  // compatibility. (No-op)
  (void)arg;
  (void)port;
  (void)tree;
  (void)result;
}

void Analyzer::check_write_param_usage(const TreeDef & tree, AnalysisResult & result)
{
  // Find parameters declared as ref/out
  std::unordered_set<std::string> writable_params;
  for (const auto & param : tree.params) {
    if (
      param.direction &&
      (*param.direction == PortDirection::Ref || *param.direction == PortDirection::Out)) {
      writable_params.insert(param.name);
    }
  }

  if (writable_params.empty()) {
    return;
  }

  // Collect parameters actually used for write access (explicit out/ref, plus
  // assignments)
  std::unordered_set<std::string> used_for_write;
  if (tree.body) {
    collect_write_usages(*tree.body, writable_params, used_for_write);
  }

  // Warn about unused write params
  for (const auto & param : tree.params) {
    if (writable_params.count(param.name) && !used_for_write.count(param.name)) {
      result.diagnostics.warning(
        param.range,
        "Parameter '" + param.name + "' is declared as ref/out but never used for write access");
    }
  }
}

void Analyzer::collect_write_usages(
  const NodeStmt & node, const std::unordered_set<std::string> & writable_params,
  std::unordered_set<std::string> & used_for_write)
{
  for (const auto & arg : node.args) {
    std::string var_name;
    std::optional<PortDirection> dir;

    // ValueExpr = variant<Literal, BlackboardRef>
    std::visit(
      [&](const auto & val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, BlackboardRef>) {
          var_name = val.name;
          dir = val.direction;
        }
      },
      arg.value);

    if (var_name.empty()) {
      continue;
    }

    // Only count explicit out/ref directions, and only for params declared as
    // writable.
    if (dir && (*dir == PortDirection::Ref || *dir == PortDirection::Out)) {
      if (writable_params.count(var_name)) {
        used_for_write.insert(var_name);
      }
    }
  }

  // Recurse into children (ChildElement = variant<Box<NodeStmt>,
  // AssignmentStmt>)
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          collect_write_usages(*elem, writable_params, used_for_write);
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          // Assignment to a writable param counts as write usage.
          if (writable_params.count(elem.target)) {
            used_for_write.insert(elem.target);
          }
        }
      },
      child);
  }
}

void Analyzer::validate_declare_stmt(const DeclareStmt & decl, AnalysisResult & result)
{
  // Check category validity
  auto category = node_category_from_string(decl.category);
  if (!category) {
    result.diagnostics.error(
      decl.range, "Invalid category: '" + decl.category + "'. " +
                    "Valid categories are: Action, Condition, "
                    "Control, Decorator, SubTree");
  }

  // Check for duplicate port names
  std::unordered_set<std::string> seen_ports;
  for (const auto & port : decl.ports) {
    auto [it, inserted] = seen_ports.insert(port.name);
    if (!inserted) {
      result.diagnostics.error(port.range, "Duplicate port name: '" + port.name + "'");
    }
  }
}

void Analyzer::validate_local_vars(
  const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result)
{
  for (const auto & local : tree.local_vars) {
    // Check that local has either type or initial value
    if (!local.type_name && !local.initial_value) {
      result.diagnostics.error(
        local.range,
        "Local variable '" + local.name + "' must have either a type annotation or initial value");
      continue;
    }

    // Check type compatibility if both are present
    if (local.type_name && local.initial_value) {
      const Type declared = Type::from_string(*local.type_name);

      // Infer type from initial value expression
      auto get_global = [](std::string_view) -> const Type * { return nullptr; };
      const TypeResolver resolver(result.symbols, result.nodes);
      auto inferred_result = resolver.infer_expression_type(*local.initial_value, ctx, get_global);

      if (!declared.is_compatible_with(inferred_result.type)) {
        result.diagnostics.error(
          local.range, "Type mismatch: cannot assign " + inferred_result.type.to_string() + " to " +
                         declared.to_string());
      }
    }
  }
}

const Type * Analyzer::get_global_var_type(std::string_view name, const Program & program)
{
  // This is a simplified lookup - in real use, would cache Type objects
  for (const auto & var : program.global_vars) {
    if (var.name == name && !var.type_name.empty()) {
      // Would need to store Type objects somewhere
      return nullptr;
    }
  }
  return nullptr;
}

bool Analyzer::is_parameter_writable(std::string_view name, const TreeDef & tree)
{
  for (const auto & param : tree.params) {
    if (param.name == name) {
      if (param.direction) {
        return *param.direction == PortDirection::Ref || *param.direction == PortDirection::Out;
      }
      return false;  // Default is input-only
    }
  }
  // Check local variables (always writable)
  for (const auto & local : tree.local_vars) {
    if (local.name == name) {
      return true;
    }
  }
  // Global variables are writable
  return true;
}

}  // namespace bt_dsl
