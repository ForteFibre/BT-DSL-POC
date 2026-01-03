// bt_dsl/sema/tree_recursion_checker.cpp - Tree call graph + recursion detection

#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bt_dsl/basic/casting.hpp"

namespace bt_dsl
{

namespace
{

struct Edge
{
  const TreeDecl * callee = nullptr;
  SourceRange callRange;
};

void collect_edges_from_stmt(const Stmt * stmt, std::vector<Edge> & out)
{
  if (!stmt) return;

  if (const auto * node = dyn_cast<NodeStmt>(stmt)) {
    if (node->resolvedNode && node->resolvedNode->is_tree()) {
      const auto * callee = cast<TreeDecl>(node->resolvedNode->decl);
      if (callee) {
        Edge e;
        e.callee = callee;
        e.callRange = node->get_range();
        out.push_back(e);
      }
    }

    for (const auto * child : node->children) {
      collect_edges_from_stmt(child, out);
    }
    return;
  }

  // Other statements have no nested statements.
}

void collect_edges_from_tree(const TreeDecl * tree, std::vector<Edge> & out)
{
  if (!tree) return;
  for (const auto * s : tree->body) {
    collect_edges_from_stmt(s, out);
  }
}

enum class Color : uint8_t { White, Gray, Black };

std::string cycle_message(gsl::span<const TreeDecl * const> stack, const TreeDecl * callee)
{
  std::string msg = "Recursive tree call is not allowed: ";
  if (!callee) {
    msg += "<unknown>";
    return msg;
  }

  size_t start = 0;
  for (; start < stack.size(); ++start) {
    if (stack[start] == callee) {
      break;
    }
  }
  if (start >= stack.size()) {
    // Fallback: stack doesn't contain callee (shouldn't happen for a back-edge)
    for (size_t i = 0; i < stack.size(); ++i) {
      if (i > 0) msg += " -> ";
      msg += std::string(stack[i]->name);
    }
    msg += " -> ";
    msg += std::string(callee->name);
    return msg;
  }

  for (size_t i = start; i < stack.size(); ++i) {
    if (i > start) msg += " -> ";
    msg += std::string(stack[i]->name);
  }
  msg += " -> ";
  msg += std::string(callee->name);
  return msg;
}

bool check_cycles(
  gsl::span<const TreeDecl * const> roots,
  const std::unordered_map<const TreeDecl *, std::vector<Edge>> & adj,
  TreeRecursionChecker & checker)
{
  std::unordered_map<const TreeDecl *, Color> color;
  color.reserve(adj.size() + 16);
  for (const auto & [k, _] : adj) {
    color.emplace(k, Color::White);
  }
  for (const auto * r : roots) {
    if (r && color.find(r) == color.end()) {
      color.emplace(r, Color::White);
    }
  }

  std::vector<const TreeDecl *> stack;
  stack.reserve(64);

  std::function<void(const TreeDecl *)> dfs;
  dfs = [&](const TreeDecl * u) {
    if (!u) return;
    color[u] = Color::Gray;
    stack.push_back(u);

    auto it_adj = adj.find(u);
    if (it_adj != adj.end()) {
      for (const auto & e : it_adj->second) {
        if (!e.callee) {
          continue;
        }

        const auto it_c = color.find(e.callee);
        const Color c = (it_c == color.end()) ? Color::White : it_c->second;

        if (c == Color::Gray) {
          const gsl::span<const TreeDecl * const> stack_view(stack.data(), stack.size());
          checker.report_error(e.callRange, cycle_message(stack_view, e.callee));
          continue;
        }
        if (c == Color::White) {
          dfs(e.callee);
        }
      }
    }

    stack.pop_back();
    color[u] = Color::Black;
  };

  for (const auto * r : roots) {
    if (!r) continue;
    auto it = color.find(r);
    if (it != color.end() && it->second == Color::White) {
      dfs(r);
    }
  }

  return !checker.has_errors();
}

}  // namespace

bool TreeRecursionChecker::check(const Program & program)
{
  hasErrors_ = false;
  errorCount_ = 0;

  // Build adjacency list for all trees in this program.
  std::unordered_map<const TreeDecl *, std::vector<Edge>> adj;
  adj.reserve(program.trees.size());

  for (const auto * t : program.trees) {
    std::vector<Edge> edges;
    edges.reserve(16);
    collect_edges_from_tree(t, edges);
    adj.emplace(t, std::move(edges));
  }

  // Roots: all trees defined in this program.
  std::vector<const TreeDecl *> roots;
  roots.reserve(program.trees.size());
  for (const auto * t : program.trees) {
    roots.push_back(t);
  }

  return check_cycles(roots, adj, *this);
}

bool TreeRecursionChecker::check(const ModuleGraph & graph, const ModuleInfo & entry)
{
  hasErrors_ = false;
  errorCount_ = 0;

  // Collect all trees across all modules in the graph.
  std::unordered_set<const TreeDecl *> all_trees;
  for (const auto * m : graph.get_all_modules()) {
    if (!m || !m->program) continue;
    for (const auto * t : m->program->trees) {
      if (t) all_trees.insert(t);
    }
  }

  // Build adjacency list.
  std::unordered_map<const TreeDecl *, std::vector<Edge>> adj;
  adj.reserve(all_trees.size());

  for (const auto * t : all_trees) {
    std::vector<Edge> edges;
    edges.reserve(16);
    collect_edges_from_tree(t, edges);

    // Filter to callee trees that are part of this compilation graph.
    std::vector<Edge> filtered;
    filtered.reserve(edges.size());
    for (const auto & e : edges) {
      if (e.callee && all_trees.count(e.callee) > 0) {
        filtered.push_back(e);
      }
    }

    adj.emplace(t, std::move(filtered));
  }

  // Roots: trees defined in entry module.
  std::vector<const TreeDecl *> roots;
  if (entry.program) {
    roots.reserve(entry.program->trees.size());
    for (const auto * t : entry.program->trees) {
      roots.push_back(t);
    }
  }

  return check_cycles(roots, adj, *this);
}

void TreeRecursionChecker::report_error(SourceRange range, std::string_view message)
{
  hasErrors_ = true;
  ++errorCount_;
  if (diags_) {
    diags_->error(range, message);
  }
}

}  // namespace bt_dsl
