#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bt_dsl/lsp/completion_context.hpp"

using bt_dsl::lsp::classify_completion_context;
using bt_dsl::lsp::CompletionContextKind;

namespace
{

uint32_t off_at(const std::string & s, const std::string & needle)
{
  const auto pos = s.find(needle);
  EXPECT_NE(pos, std::string::npos);
  return static_cast<uint32_t>(pos);
}

}  // namespace

TEST(LspCompletionContext, ImportPathInsideString)
{
  const std::string src = "import \"std/nodes.bt\";\n";
  const uint32_t off = off_at(src, "std/");

  auto ctx = classify_completion_context(src, off);
  if (!ctx.has_value()) {
    ADD_FAILURE() << "expected a completion context";
    return;
  }
  const auto & ctxv = *ctx;
  EXPECT_EQ(ctxv.kind, CompletionContextKind::ImportPath);
}

TEST(LspCompletionContext, PreconditionKindAfterAt)
{
  const std::string src = "tree T() {\n  @guard(x)\n  AlwaysSuccess();\n}\n";
  const uint32_t off = off_at(src, "@guard") + 1;  // right after '@'

  auto ctx = classify_completion_context(src, off);
  if (!ctx.has_value()) {
    ADD_FAILURE() << "expected a completion context";
    return;
  }
  const auto & ctxv = *ctx;
  EXPECT_EQ(ctxv.kind, CompletionContextKind::PreconditionKind);
}

TEST(LspCompletionContext, TreeBodyInsideBraces)
{
  const std::string src = "tree MyTree() {\n  \n}\n";
  const uint32_t off = off_at(src, "\n  \n") + 3;  // inside body

  auto ctx = classify_completion_context(src, off);
  if (!ctx.has_value()) {
    ADD_FAILURE() << "expected a completion context";
    return;
  }
  const auto & ctxv = *ctx;
  EXPECT_EQ(ctxv.kind, CompletionContextKind::TreeBody);
  if (!ctxv.tree_name.has_value()) {
    ADD_FAILURE() << "expected tree_name to be present";
    return;
  }
  EXPECT_EQ(*ctxv.tree_name, "MyTree");
}

TEST(LspCompletionContext, ArgValueAfterColon)
{
  const std::string src = "tree T() {\n  Foo(a: 1, b: 2);\n}\n";
  const uint32_t off = off_at(src, "a:") + 2;  // after ':'

  auto ctx = classify_completion_context(src, off);
  if (!ctx.has_value()) {
    ADD_FAILURE() << "expected a completion context";
    return;
  }
  const auto & ctxv = *ctx;
  // Still inside parens; after ':' should be value context.
  EXPECT_EQ(ctxv.kind, CompletionContextKind::ArgValue);
}

TEST(LspCompletionContext, ArgStartAfterLParen)
{
  const std::string src = "tree T() {\n  Foo(\n}\n";
  const uint32_t off = off_at(src, "Foo(") + 4;

  auto ctx = classify_completion_context(src, off);
  if (!ctx.has_value()) {
    ADD_FAILURE() << "expected a completion context";
    return;
  }
  const auto & ctxv = *ctx;
  EXPECT_EQ(ctxv.kind, CompletionContextKind::ArgStart);
}
