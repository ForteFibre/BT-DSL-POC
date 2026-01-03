// bt_dsl/sema/analysis/cfg.hpp - Control Flow Graph for BT-DSL
//
// CFG data structures for data-flow analysis (initialization checking,
// null safety). Designed for BT semantics with Success/Failure outcomes.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"

namespace bt_dsl
{

// ============================================================================
// CFG Edge Kinds
// ============================================================================

/**
 * Edge kind for CFG edges.
 *
 * BT nodes have Success/Failure outcomes that determine control flow:
 * - Sequence (DataPolicy::All): continues on Success, exits on Failure
 * - Fallback (DataPolicy::Any): continues on Failure, exits on Success
 */
enum class CFGEdgeKind : uint8_t {
  // Basic edges
  Unconditional,  ///< Always taken (sequential flow)

  // Precondition edges (guard conditions)
  GuardTrue,   ///< Condition evaluated to true (enter node body)
  GuardFalse,  ///< Condition evaluated to false (skip node)

  // BT result-based edges (child node outcomes)
  ChildSuccess,  ///< Child returned Success
  ChildFailure,  ///< Child returned Failure

  // Parent exit edges
  ParentSuccess,  ///< Parent node exits with Success
  ParentFailure,  ///< Parent node exits with Failure
};

// ============================================================================
// Basic Block
// ============================================================================

/**
 * A basic block in the CFG.
 *
 * Contains a sequence of statements that execute linearly without branching.
 */
struct BasicBlock
{
  /// Unique identifier within the CFG
  size_t id = 0;

  /// Statements in this block (executed sequentially)
  std::vector<const Stmt *> stmts;

  /// An outgoing edge from this block
  struct Edge
  {
    BasicBlock * target = nullptr;
    CFGEdgeKind kind = CFGEdgeKind::Unconditional;
    const Expr * condition = nullptr;  ///< For Guard edges: the condition expression
  };

  /// Outgoing edges
  std::vector<Edge> successors;

  /// Incoming edges (predecessors only, no edge metadata needed)
  std::vector<BasicBlock *> predecessors;

  // Block metadata for data-flow analysis
  DataPolicy dataPolicy = DataPolicy::All;
  FlowPolicy flowPolicy = FlowPolicy::Chained;
  const NodeStmt * parentNode = nullptr;  ///< Owning node (for children blocks)
  BasicBlock * contextEntry = nullptr;  ///< Entry block of the current context (for Isolated reset)

  /// Add an outgoing edge
  void add_successor(
    BasicBlock * target, CFGEdgeKind kind = CFGEdgeKind::Unconditional,
    const Expr * condition = nullptr)
  {
    successors.push_back({target, kind, condition});
    if (target != nullptr) {
      target->predecessors.push_back(this);
    }
  }
};

// ============================================================================
// Control Flow Graph
// ============================================================================

/**
 * Control Flow Graph for a single TreeDecl.
 *
 * Provides the structure for forward data-flow analysis:
 * - Initialization checking (§6.1)
 * - Null safety / narrowing (§6.2)
 */
class CFG
{
public:
  CFG() = default;

  // Non-copyable, movable
  CFG(const CFG &) = delete;
  CFG & operator=(const CFG &) = delete;
  CFG(CFG &&) = default;
  CFG & operator=(CFG &&) = default;

  /// Entry block (start of tree execution)
  BasicBlock * entry = nullptr;

  /// Exit block when tree returns Success
  BasicBlock * exitSuccess = nullptr;

  /// Exit block when tree returns Failure
  BasicBlock * exitFailure = nullptr;

  /// All blocks in this CFG
  std::vector<std::unique_ptr<BasicBlock>> blocks;

  /// The tree this CFG was built from
  const TreeDecl * tree = nullptr;

  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /**
   * Create a new basic block.
   * @return Pointer to the newly created block
   */
  BasicBlock * create_block()
  {
    auto block = std::make_unique<BasicBlock>();
    block->id = blocks.size();
    BasicBlock * ptr = block.get();
    blocks.push_back(std::move(block));
    return ptr;
  }

  /**
   * Add a statement to a block.
   */
  static void add_stmt(BasicBlock * block, const Stmt * stmt)
  {
    if (block != nullptr && stmt != nullptr) {
      block->stmts.push_back(stmt);
    }
  }

  /**
   * Get the number of blocks.
   */
  [[nodiscard]] size_t size() const noexcept { return blocks.size(); }

  /**
   * Check if the CFG is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return blocks.empty(); }
};

}  // namespace bt_dsl
