// bt_dsl/init_safety.cpp - Blackboard initialization safety analysis
#include "bt_dsl/semantic/init_safety.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bt_dsl
{

namespace
{

enum class InitTri : uint8_t {
  Uninit,
  Init,
  Unknown,
};

struct EnvFrame
{
  std::unordered_map<std::string, InitTri> vars;
  std::unordered_set<std::string> consts;
};

struct Env
{
  // globals are not part of the lexical frame stack
  std::unordered_map<std::string, InitTri> globals;
  std::unordered_set<std::string> global_consts;

  std::vector<EnvFrame> frames;

  // parameters (by name) for this tree
  std::unordered_set<std::string> params;
  std::unordered_set<std::string> out_params;

  // requirements inferred under Unknown
  std::unordered_set<std::string> required_globals;

  bool infer_mode = false;  // if true, Unknown reads become requirements
};

bool is_public_name(std::string_view name) { return !name.empty() && name.front() != '_'; }

EnvFrame & current_frame(Env & env)
{
  if (env.frames.empty()) {
    env.frames.emplace_back();
  }
  return env.frames.back();
}

void push_frame(Env & env) { env.frames.emplace_back(); }
void pop_frame(Env & env)
{
  if (!env.frames.empty()) {
    env.frames.pop_back();
  }
}

bool is_const_name(const Env & env, std::string_view name)
{
  const std::string key(name);
  for (auto it = env.frames.rbegin(); it != env.frames.rend(); ++it) {
    if (it->consts.count(key)) {
      return true;
    }
  }
  return env.global_consts.count(key) > 0;
}

std::optional<InitTri *> lookup_var_state(Env & env, std::string_view name)
{
  const std::string key(name);
  for (auto it = env.frames.rbegin(); it != env.frames.rend(); ++it) {
    auto vit = it->vars.find(key);
    if (vit != it->vars.end()) {
      return &vit->second;
    }
  }

  auto git = env.globals.find(key);
  if (git != env.globals.end()) {
    return &git->second;
  }

  return std::nullopt;
}

bool is_global_name(const Env & env, std::string_view name)
{
  return env.globals.count(std::string(name)) > 0;
}

bool require_init(
  Env & env, std::string_view name, const SourceRange & range, std::string_view what,
  DiagnosticBag & diagnostics)
{
  // constants are always initialized
  if (is_const_name(env, name)) {
    return true;
  }

  auto st_ptr = lookup_var_state(env, name);
  if (!st_ptr.has_value() || !(*st_ptr)) {
    return true;  // undefined vars are handled by other passes
  }

  InitTri & st = *(*st_ptr);
  switch (st) {
    case InitTri::Init:
      return true;
    case InitTri::Uninit:
      diagnostics.error(
        range, "Blackboard variable '" + std::string(name) +
                 "' may be uninitialized when used as " + std::string(what));
      return false;
    case InitTri::Unknown:
      if (env.infer_mode) {
        if (is_global_name(env, name)) {
          env.required_globals.insert(std::string(name));
        }
        // assume initialized under the requirement
        st = InitTri::Init;
        return true;
      }
      diagnostics.error(
        range, "Blackboard variable '" + std::string(name) +
                 "' may be uninitialized when used as " + std::string(what));
      return false;
  }
  return true;
}

void mark_init(Env & env, std::string_view name)
{
  if (is_const_name(env, name)) {
    return;
  }

  auto st_ptr = lookup_var_state(env, name);
  if (!st_ptr.has_value() || !(*st_ptr)) {
    return;
  }
  *(*st_ptr) = InitTri::Init;
}

void declare_var(Env & env, std::string_view name, InitTri st)
{
  EnvFrame & f = current_frame(env);
  f.vars.insert_or_assign(std::string(name), st);
}

void declare_const(Env & env, std::string_view name)
{
  EnvFrame & f = current_frame(env);
  f.consts.insert(std::string(name));
}

std::optional<std::string> extract_lvalue_base_name(const Expression & expr)
{
  if (const auto * vr = std::get_if<VarRef>(&expr)) {
    return vr->name;
  }
  if (const auto * idx = std::get_if<Box<IndexExpr>>(&expr)) {
    return extract_lvalue_base_name((*idx)->base);
  }
  return std::nullopt;
}

void check_expr_reads(const Expression & expr, Env & env, DiagnosticBag & diagnostics)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        require_init(env, e.name, e.range, "an expression", diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        check_expr_reads(e->left, env, diagnostics);
        check_expr_reads(e->right, env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        check_expr_reads(e->operand, env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        check_expr_reads(e->expr, env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        check_expr_reads(e->base, env, diagnostics);
        check_expr_reads(e->index, env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          check_expr_reads(el, env, diagnostics);
        }
        if (e->repeat_value) {
          check_expr_reads(*e->repeat_value, env, diagnostics);
        }
        if (e->repeat_count) {
          check_expr_reads(*e->repeat_count, env, diagnostics);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        // vec![...] is an ArrayLiteralExpr inside.
        for (const auto & el : e->value.elements) {
          check_expr_reads(el, env, diagnostics);
        }
        if (e->value.repeat_value) {
          check_expr_reads(*e->value.repeat_value, env, diagnostics);
        }
        if (e->value.repeat_count) {
          check_expr_reads(*e->value.repeat_count, env, diagnostics);
        }
        return;
      }
    },
    expr);
}

void check_out_target_safety(const Expression & expr, Env & env, DiagnosticBag & diagnostics)
{
  // For out ports, the lvalue itself does not need to be initialized.
  // However, indexed writes like out a[i] require that the base container `a`
  // is initialized and that the index expression is safe.
  if (const auto * vr = std::get_if<VarRef>(&expr)) {
    (void)vr;
    return;
  }
  if (const auto * idx = std::get_if<Box<IndexExpr>>(&expr)) {
    // index expression must be safe
    check_expr_reads((*idx)->index, env, diagnostics);
    if (auto base = extract_lvalue_base_name((*idx)->base)) {
      require_init(env, *base, (*idx)->range, "an out target base", diagnostics);
    }
    return;
  }

  // Fallback: be conservative.
  check_expr_reads(expr, env, diagnostics);
}

void collect_var_decls_recursive(
  const std::vector<Statement> & stmts, std::unordered_set<std::string> & out)
{
  for (const auto & s : stmts) {
    std::visit(
      [&](const auto & st) {
        using T = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
          out.insert(st.name);
        } else if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          for (const auto & arg : st->args) {
            if (const auto * inl = std::get_if<InlineBlackboardDecl>(&arg.value)) {
              out.insert(inl->name);
            }
          }
          if (st->has_children_block) {
            collect_var_decls_recursive(st->children, out);
          }
        }
      },
      s);
  }
}

struct TreeContext
{
  const TreeDef * def = nullptr;
  const Program * owner = nullptr;
};

struct TreeSummaries
{
  std::unordered_map<std::string, InitSafetySummary> by_name;
};

struct AnalyzeResult
{
  // Variables guaranteed Init if this statement (or block) succeeds.
  std::unordered_set<std::string> writes_on_success;
};

std::unordered_set<std::string> set_union(
  const std::unordered_set<std::string> & a, const std::unordered_set<std::string> & b)
{
  std::unordered_set<std::string> out = a;
  for (const auto & x : b) {
    out.insert(x);
  }
  return out;
}

std::unordered_set<std::string> set_intersection(
  const std::unordered_set<std::string> & a, const std::unordered_set<std::string> & b)
{
  std::unordered_set<std::string> out;
  if (a.size() < b.size()) {
    for (const auto & x : a) {
      if (b.count(x)) {
        out.insert(x);
      }
    }
  } else {
    for (const auto & x : b) {
      if (a.count(x)) {
        out.insert(x);
      }
    }
  }
  return out;
}

const PortInfo * resolve_port(
  const NodeInfo & info, const Argument & arg, std::optional<std::string> & out_port_name)
{
  out_port_name.reset();

  // Reference: docs/reference/syntax.md 2.6.4
  // Positional arguments are not supported.
  if (!arg.name) {
    return nullptr;
  }

  out_port_name = *arg.name;
  return info.get_port(*arg.name);
}

std::optional<std::string> extract_written_var_from_argument_value(const ArgumentValue & v)
{
  if (const auto * inl = std::get_if<InlineBlackboardDecl>(&v)) {
    return inl->name;
  }
  if (const auto * expr = std::get_if<Expression>(&v)) {
    // out/ref/mut require lvalue. For init safety we only model base variable.
    return extract_lvalue_base_name(*expr);
  }
  return std::nullopt;
}

void ensure_inline_decl_registered(
  Env & env, const InlineBlackboardDecl & decl, bool in_isolated_block, DiagnosticBag & diagnostics)
{
  (void)in_isolated_block;
  (void)diagnostics;
  // New variable starts Uninit.
  declare_var(env, decl.name, InitTri::Uninit);
}

AnalyzeResult analyze_statement_list(
  const std::vector<Statement> & stmts, Env & env, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  bool in_isolated_block, DiagnosticBag & diagnostics);

AnalyzeResult analyze_statement(
  const Statement & stmt, Env & env, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  bool in_isolated_block, DiagnosticBag & diagnostics);

AnalyzeResult analyze_node_stmt(
  const NodeStmt & node, Env & env, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  bool in_isolated_block, DiagnosticBag & diagnostics)
{
  AnalyzeResult res;

  // Reference: docs/reference/execution-model.md (Preconditions)
  // Some preconditions can cause the node body/children to be skipped while the parent
  // may still proceed to subsequent siblings (e.g. Skip treated like Success).
  // Therefore, no out-writes are guaranteed on success when any such precondition is present.
  bool precond_can_skip_body = false;
  for (const auto & pc : node.preconditions) {
    if (pc.kind == "success_if" || pc.kind == "skip_if" || pc.kind == "run_while") {
      precond_can_skip_body = true;
      break;
    }
  }

  const NodeInfo * info = nodes.get_node(node.node_name);
  if (!info) {
    return res;
  }

  // Preconditions are evaluated before the node runs; reads must be initialized.
  for (const auto & pc : node.preconditions) {
    check_expr_reads(pc.condition, env, diagnostics);
  }

  // 1) Check arguments (read requirements) and process inline declarations.
  for (const auto & arg : node.args) {
    std::optional<std::string> port_name;
    const PortInfo * port = resolve_port(*info, arg, port_name);
    if (!port) {
      continue;
    }

    // inline decl form: out var x
    if (const auto * inl = std::get_if<InlineBlackboardDecl>(&arg.value)) {
      ensure_inline_decl_registered(env, *inl, in_isolated_block, diagnostics);
      // out allows Uninit
      continue;
    }

    const auto * expr = std::get_if<Expression>(&arg.value);
    if (!expr) {
      continue;
    }

    if (port->direction == PortDirection::Out) {
      check_out_target_safety(*expr, env, diagnostics);
    } else {
      // in/ref/mut: any blackboard read must be initialized.
      check_expr_reads(*expr, env, diagnostics);
    }
  }

  // 2) Collect guaranteed writes from this node on success.
  // - For leaf nodes (including declared actions) we assume out ports are written on success.
  // - For SubTree calls, out writes are refined by the callee summary.
  std::unordered_set<std::string> own_out_writes;
  for (const auto & arg : node.args) {
    std::optional<std::string> port_name;
    const PortInfo * port = resolve_port(*info, arg, port_name);
    if (!port || port->direction != PortDirection::Out) {
      continue;
    }

    auto written = extract_written_var_from_argument_value(arg.value);
    if (!written) {
      continue;
    }
    own_out_writes.insert(*written);
  }

  // SubTree refinement: only out params that are guaranteed by the callee become Init.
  if (info->category == NodeCategory::SubTree) {
    auto it_tree = summaries.by_name.find(node.node_name);
    if (it_tree != summaries.by_name.end()) {
      const auto & sum = it_tree->second;
      std::unordered_set<std::string> refined;
      refined.reserve(own_out_writes.size());

      // Map callee out-param names to caller argument variable names.
      for (const auto & arg : node.args) {
        std::optional<std::string> port_name;
        const PortInfo * port = resolve_port(*info, arg, port_name);
        if (!port || port->direction != PortDirection::Out || !port_name) {
          continue;
        }
        if (!sum.writes_out_params_on_success.count(*port_name)) {
          continue;
        }
        auto written = extract_written_var_from_argument_value(arg.value);
        if (written) {
          refined.insert(*written);
        }
      }
      own_out_writes = std::move(refined);
    }
  }

  // Apply own writes to env and result.
  if (!precond_can_skip_body) {
    for (const auto & w : own_out_writes) {
      mark_init(env, w);
      res.writes_on_success.insert(w);
    }
  }

  // 3) Analyze children for Control/Decorator nodes.
  if (!node.has_children_block) {
    return res;
  }

  const bool children_isolated = (info->behavior.flow == FlowPolicy::Isolated);
  if (children_isolated) {
    // In isolated blocks, do not assume ordering or sibling success. To keep the analysis
    // sound and conservative, treat any var/inline-decl that appears anywhere in the block
    // as declared-but-uninitialized at entry.
    std::unordered_set<std::string> names;
    collect_var_decls_recursive(node.children, names);
    for (const auto & n : names) {
      declare_var(env, n, InitTri::Uninit);
    }
  }

  // Analyze each child statement as a child-node in BT.CPP.
  // XML codegen turns each statement into either a Node, or a Script node.
  // Therefore, policy evaluation is over this statement list.
  const Env pre_env = env;

  std::vector<std::unordered_set<std::string>> child_writes;
  child_writes.reserve(node.children.size());

  auto analyze_child = [&](const Statement & child_stmt, Env & child_env) -> AnalyzeResult {
    push_frame(child_env);
    auto r = analyze_statement(
      child_stmt, child_env, nodes, trees, summaries, in_isolated_block || children_isolated,
      diagnostics);
    pop_frame(child_env);
    return r;
  };

  if (info->behavior.flow == FlowPolicy::Chained && info->behavior.data == DataPolicy::All) {
    // Children execute in order and each child (when reached) implies previous children succeeded.
    // Therefore we can safely propagate guaranteed writes from previous children.
    Env chained_env = env;
    for (const auto & child_stmt : node.children) {
      auto r = analyze_child(child_stmt, chained_env);
      child_writes.push_back(r.writes_on_success);
      for (const auto & w : r.writes_on_success) {
        mark_init(chained_env, w);
      }
    }
  } else {
    // Any/None and/or Isolated: do not assume previous children succeeded.
    for (const auto & child_stmt : node.children) {
      Env child_env = pre_env;
      auto r = analyze_child(child_stmt, child_env);
      child_writes.push_back(std::move(r.writes_on_success));
    }
  }

  std::unordered_set<std::string> merged;
  if (info->behavior.data == DataPolicy::All) {
    for (const auto & s : child_writes) {
      merged = set_union(merged, s);
    }
  } else if (info->behavior.data == DataPolicy::Any) {
    if (!child_writes.empty()) {
      merged = child_writes.front();
      for (size_t i = 1; i < child_writes.size(); ++i) {
        merged = set_intersection(merged, child_writes[i]);
      }
    }
  } else {
    // None
    merged.clear();
  }

  // Apply merged writes to env and result.
  if (!precond_can_skip_body) {
    for (const auto & w : merged) {
      mark_init(env, w);
      res.writes_on_success.insert(w);
    }
  }

  return res;
}

AnalyzeResult analyze_statement(
  const Statement & stmt, Env & env, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  bool in_isolated_block, DiagnosticBag & diagnostics)
{
  AnalyzeResult res;

  std::visit(
    [&](const auto & s) {
      using T = std::decay_t<decltype(s)>;
      if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
        res = analyze_node_stmt(*s, env, nodes, trees, summaries, in_isolated_block, diagnostics);
      } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
        // Reference: docs/reference/execution-model.md 5.3.3 (Preconditions)
        // Some preconditions may skip the statement body while the parent can
        // still proceed (e.g. treated like Success/Skip), therefore the write
        // is not guaranteed on the success path in those cases.
        bool precond_can_skip_body = false;
        for (const auto & pc : s.preconditions) {
          if (pc.kind == "success_if" || pc.kind == "skip_if" || pc.kind == "run_while") {
            precond_can_skip_body = true;
            break;
          }
        }

        for (const auto & pc : s.preconditions) {
          check_expr_reads(pc.condition, env, diagnostics);
        }

        // RHS reads must be initialized.
        check_expr_reads(s.value, env, diagnostics);
        for (const auto & idx : s.indices) {
          check_expr_reads(idx, env, diagnostics);
        }
        // Assignment initializes the target only if the statement body is guaranteed
        // to run on the success path.
        if (!precond_can_skip_body) {
          mark_init(env, s.target);
          res.writes_on_success.insert(s.target);
        }
      } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
        (void)in_isolated_block;

        if (s.initial_value) {
          check_expr_reads(*s.initial_value, env, diagnostics);
          declare_var(env, s.name, InitTri::Init);
          res.writes_on_success.insert(s.name);
        } else {
          declare_var(env, s.name, InitTri::Uninit);
        }
      } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
        check_expr_reads(s.value, env, diagnostics);
        declare_const(env, s.name);
        // const is always initialized
      }
    },
    stmt);

  return res;
}

AnalyzeResult analyze_statement_list(
  const std::vector<Statement> & stmts, Env & env, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  bool in_isolated_block, DiagnosticBag & diagnostics)
{
  AnalyzeResult acc;

  for (const auto & stmt : stmts) {
    auto r = analyze_statement(stmt, env, nodes, trees, summaries, in_isolated_block, diagnostics);
    acc.writes_on_success = set_union(acc.writes_on_success, r.writes_on_success);
  }

  return acc;
}

std::unordered_map<std::string, TreeContext> collect_all_trees(
  const Program & program, const std::vector<const Program *> & imported_programs)
{
  std::unordered_map<std::string, TreeContext> trees;
  trees.reserve(program.trees.size() + 32);

  for (const auto & t : program.trees) {
    trees.emplace(t.name, TreeContext{&t, &program});
  }

  for (const auto * imp : imported_programs) {
    if (!imp) {
      continue;
    }
    for (const auto & t : imp->trees) {
      trees.emplace(t.name, TreeContext{&t, imp});
    }
  }

  return trees;
}

std::unordered_map<std::string, std::unordered_set<std::string>> build_tree_call_graph(
  const std::unordered_map<std::string, TreeContext> & trees)
{
  // caller -> set(callee)
  std::unordered_map<std::string, std::unordered_set<std::string>> g;
  g.reserve(trees.size());

  std::function<void(const Statement &, std::unordered_set<std::string> &)> walk_stmt;
  walk_stmt = [&](const Statement & s, std::unordered_set<std::string> & out) {
    if (const auto * n = std::get_if<Box<NodeStmt>>(&s)) {
      const auto * node = n->get();
      if (!node) {
        return;
      }
      out.insert(node->node_name);
      for (const auto & ch : node->children) {
        walk_stmt(ch, out);
      }
    }
  };

  for (const auto & [caller, ctx] : trees) {
    if (!ctx.def || !ctx.owner) {
      continue;
    }

    std::unordered_set<std::string> callees_raw;
    for (const auto & s : ctx.def->body) {
      walk_stmt(s, callees_raw);
    }

    std::unordered_set<std::string> callees;
    for (const auto & callee : callees_raw) {
      auto it = trees.find(callee);
      if (it == trees.end()) {
        continue;
      }

      const bool callee_public = is_public_name(callee);
      const bool same_file = (it->second.owner == ctx.owner);
      if (!callee_public && !same_file) {
        continue;
      }

      callees.insert(callee);
    }

    g.emplace(caller, std::move(callees));
  }

  return g;
}

std::vector<std::string> topo_sort_trees(
  const std::unordered_map<std::string, std::unordered_set<std::string>> & g)
{
  enum class Mark : uint8_t { White, Gray, Black };
  std::unordered_map<std::string, Mark> mark;
  mark.reserve(g.size());

  for (const auto & [u, _] : g) {
    mark.emplace(u, Mark::White);
  }

  std::vector<std::string> order;
  order.reserve(g.size());

  std::function<void(const std::string &)> dfs;
  dfs = [&](const std::string & u) {
    auto it_m = mark.find(u);
    if (it_m == mark.end()) {
      return;
    }
    if (it_m->second != Mark::White) {
      return;
    }
    it_m->second = Mark::Gray;

    auto it_g = g.find(u);
    if (it_g != g.end()) {
      for (const auto & v : it_g->second) {
        dfs(v);
      }
    }

    it_m->second = Mark::Black;
    order.push_back(u);
  };

  for (const auto & [u, _] : g) {
    dfs(u);
  }

  // postorder already yields callees before callers
  return order;
}

Env build_tree_env_for_inference(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const TreeDef & tree)
{
  Env env;
  env.infer_mode = true;
  env.frames.emplace_back();

  // Global consts
  for (const auto & gc : program.global_consts) {
    env.global_consts.insert(gc.name);
  }
  for (const auto * imp : imported_programs) {
    if (!imp) {
      continue;
    }
    for (const auto & gc : imp->global_consts) {
      if (!is_public_name(gc.name)) {
        continue;
      }
      env.global_consts.insert(gc.name);
    }
  }

  // Globals: Init if explicitly initialized, else Unknown.
  for (const auto & gv : program.global_vars) {
    env.globals.emplace(gv.name, gv.initial_value ? InitTri::Init : InitTri::Unknown);
  }
  for (const auto * imp : imported_programs) {
    if (!imp) {
      continue;
    }
    for (const auto & gv : imp->global_vars) {
      if (!is_public_name(gv.name)) {
        continue;
      }
      env.globals.emplace(gv.name, gv.initial_value ? InitTri::Init : InitTri::Unknown);
    }
  }

  // Params
  for (const auto & p : tree.params) {
    env.params.insert(p.name);
    const PortDirection dir = p.direction.value_or(PortDirection::In);
    if (dir == PortDirection::Out) {
      env.out_params.insert(p.name);
      declare_var(env, p.name, InitTri::Uninit);
    } else {
      declare_var(env, p.name, InitTri::Init);
    }
  }

  return env;
}

Env build_tree_env_for_entry_check(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const TreeDef & tree)
{
  Env env;
  env.infer_mode = false;
  env.frames.emplace_back();

  // Global consts
  for (const auto & gc : program.global_consts) {
    env.global_consts.insert(gc.name);
  }
  for (const auto * imp : imported_programs) {
    if (!imp) {
      continue;
    }
    for (const auto & gc : imp->global_consts) {
      if (!is_public_name(gc.name)) {
        continue;
      }
      env.global_consts.insert(gc.name);
    }
  }

  // Globals: Init if explicitly initialized, else Uninit at entry.
  for (const auto & gv : program.global_vars) {
    env.globals.emplace(gv.name, gv.initial_value ? InitTri::Init : InitTri::Uninit);
  }
  for (const auto * imp : imported_programs) {
    if (!imp) {
      continue;
    }
    for (const auto & gv : imp->global_vars) {
      if (!is_public_name(gv.name)) {
        continue;
      }
      env.globals.emplace(gv.name, gv.initial_value ? InitTri::Init : InitTri::Uninit);
    }
  }

  // Params at entry: treat in/ref/mut as Init, out as Uninit.
  for (const auto & p : tree.params) {
    env.params.insert(p.name);
    const PortDirection dir = p.direction.value_or(PortDirection::In);
    if (dir == PortDirection::Out) {
      env.out_params.insert(p.name);
      declare_var(env, p.name, InitTri::Uninit);
    } else {
      declare_var(env, p.name, InitTri::Init);
    }
  }

  return env;
}

InitSafetySummary infer_tree_summary(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const TreeDef & tree, const NodeRegistry & nodes,
  const std::unordered_map<std::string, TreeContext> & trees, const TreeSummaries & summaries,
  DiagnosticBag & diagnostics)
{
  Env env = build_tree_env_for_inference(program, imported_programs, tree);

  auto r = analyze_statement_list(tree.body, env, nodes, trees, summaries, false, diagnostics);

  InitSafetySummary sum;
  sum.requires_globals = std::move(env.required_globals);

  // Project writes into globals and out params.
  for (const auto & w : r.writes_on_success) {
    if (env.globals.count(w)) {
      sum.writes_globals_on_success.insert(w);
    }
    if (env.out_params.count(w)) {
      sum.writes_out_params_on_success.insert(w);
    }
  }

  return sum;
}

}  // namespace

void run_initialization_safety(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const NodeRegistry & nodes, DiagnosticBag & diagnostics)
{
  if (program.trees.empty()) {
    return;
  }

  const auto trees = collect_all_trees(program, imported_programs);
  const auto graph = build_tree_call_graph(trees);
  const auto order = topo_sort_trees(graph);

  TreeSummaries summaries;
  summaries.by_name.reserve(trees.size());

  // Infer summaries bottom-up.
  for (const auto & name : order) {
    auto it = trees.find(name);
    if (it == trees.end() || !it->second.def) {
      continue;
    }

    summaries.by_name.insert_or_assign(
      name, infer_tree_summary(
              program, imported_programs, *it->second.def, nodes, trees, summaries, diagnostics));
  }

  // Entry tree check (matches XML generator: first tree is the main entry).
  const TreeDef & entry = program.trees.front();
  {
    Env env = build_tree_env_for_entry_check(program, imported_programs, entry);

    // Ensure globals required by subtrees are satisfied at call sites.
    analyze_statement_list(entry.body, env, nodes, trees, summaries, false, diagnostics);
  }
}

}  // namespace bt_dsl
