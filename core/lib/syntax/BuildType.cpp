// bt_dsl/syntax/BuildType.cpp - CST -> AST for types
#include <string_view>

#include "bt_dsl/syntax/ast_builder.hpp"

namespace bt_dsl
{

static std::string_view strip_trailing_cr(std::string_view s)
{
  if (!s.empty() && s.back() == '\r') return s.substr(0, s.size() - 1);
  return s;
}

// Helper: find if an (unnamed) child with a given kind exists.
static bool has_child_kind(ts_ll::Node n, std::string_view kind)
{
  for (uint32_t i = 0; i < n.child_count(); ++i) {
    if (n.child(i).kind() == kind) return true;
  }
  return false;
}

TypeExpr * AstBuilder::build_type(ts_ll::Node type_node)
{
  if (type_node.is_null()) {
    diags_.error({}, "Missing type node");
    return ast_.create<TypeExpr>(ast_.create<InferType>());
  }

  // Peel wrappers that just forward to a single named child.
  auto peel_single_named = [&](ts_ll::Node n) -> ts_ll::Node {
    if (n.named_child_count() == 1) return n.named_child(0);
    return n;
  };

  // type = base_type , ["?"]
  if (type_node.kind() == "type") {
    const ts_ll::Node base_type =
      type_node.named_child_count() > 0 ? type_node.named_child(0) : ts_ll::Node{};
    if (base_type.is_null() || base_type.kind() != "base_type") {
      diags_.error(node_range(type_node), "type missing base_type");
      auto * infer = ast_.create<InferType>(node_range(type_node));
      return ast_.create<TypeExpr>(infer, /*nullable*/ false, node_range(type_node));
    }

    // base_type has a single named child: primary/static_array/dynamic/infer
    const ts_ll::Node base = peel_single_named(base_type);

    TypeNode * built_base = nullptr;

    if (base.kind() == "primary_type") {
      // primary_type = identifier | bounded_string | 'string'
      if (base.named_child_count() == 1 && base.named_child(0).kind() == "bounded_string") {
        const ts_ll::Node bs = base.named_child(0);
        const ts_ll::Node max_len = bs.child_by_field("max_len");
        auto name = ast_.intern("string");
        if (max_len.is_null()) {
          built_base = ast_.create<PrimaryType>(name, node_range(bs));
        } else {
          auto size = ast_.intern(strip_trailing_cr(max_len.text(sm_)));
          built_base = ast_.create<PrimaryType>(name, size, node_range(bs));
        }
      } else if (base.named_child_count() == 1 && base.named_child(0).kind() == "identifier") {
        const ts_ll::Node id = base.named_child(0);
        auto raw = strip_trailing_cr(id.text(sm_));
        if (raw == "_") {
          built_base = ast_.create<InferType>(node_range(id));
        } else {
          auto nm = intern_text(id);
          built_base = ast_.create<PrimaryType>(nm, node_range(base));
        }
      } else {
        // Might be the keyword 'string' alternative (no named children).
        auto txt = strip_trailing_cr(base.text(sm_));
        if (txt == "string") {
          built_base = ast_.create<PrimaryType>(ast_.intern("string"), node_range(base));
        } else {
          diags_.error(node_range(base), "Unsupported primary_type");
          built_base = ast_.create<PrimaryType>(ast_.intern("_"), node_range(base));
        }
      }
    } else if (base.kind() == "bounded_string") {
      // In case grammar emits bounded_string directly.
      const ts_ll::Node max_len = base.child_by_field("max_len");
      auto name = ast_.intern("string");
      if (max_len.is_null()) {
        built_base = ast_.create<PrimaryType>(name, node_range(base));
      } else {
        auto size = ast_.intern(strip_trailing_cr(max_len.text(sm_)));
        built_base = ast_.create<PrimaryType>(name, size, node_range(base));
      }
    } else if (base.kind() == "static_array_type") {
      const ts_ll::Node elem = base.child_by_field("element");
      const ts_ll::Node size_spec = base.child_by_field("size");
      if (elem.is_null() || size_spec.is_null()) {
        diags_.error(node_range(base), "static_array_type missing element/size");
        built_base = ast_.create<InferType>(node_range(base));
      } else {
        TypeExpr * elem_ty = build_type(elem);
        // array_size_spec = exact: array_size | '<=' max: array_size
        const ts_ll::Node exact = size_spec.child_by_field("exact");
        const ts_ll::Node max = size_spec.child_by_field("max");
        bool bounded = false;
        ts_ll::Node size_node;
        if (!max.is_null()) {
          bounded = true;
          size_node = max;
        } else if (!exact.is_null()) {
          bounded = false;
          size_node = exact;
        } else {
          // Fallback: take first named child if present
          bounded = has_child_kind(size_spec, "<=");
          size_node = size_spec.named_child_count() > 0 ? size_spec.named_child(0) : ts_ll::Node{};
        }

        if (size_node.is_null()) {
          diags_.error(node_range(size_spec), "array_size_spec missing size");
          built_base =
            ast_.create<StaticArrayType>(elem_ty, ast_.intern("0"), bounded, node_range(base));
        } else {
          auto sz = ast_.intern(strip_trailing_cr(size_node.text(sm_)));
          built_base = ast_.create<StaticArrayType>(elem_ty, sz, bounded, node_range(base));
        }
      }
    } else if (base.kind() == "dynamic_array_type") {
      const ts_ll::Node elem = base.child_by_field("element");
      if (elem.is_null()) {
        diags_.error(node_range(base), "dynamic_array_type missing element");
        built_base =
          ast_.create<DynamicArrayType>(ast_.create<InferType>(node_range(base)), node_range(base));
      } else {
        TypeExpr * elem_ty = build_type(elem);
        built_base = ast_.create<DynamicArrayType>(elem_ty, node_range(base));
      }
    } else if (base.kind() == "infer_type" || base.kind() == "infer_type_wildcard") {
      built_base = ast_.create<InferType>(node_range(base));
    } else if (base.kind() == "base_type") {
      // Defensive: peel again.
      const ts_ll::Node inner = peel_single_named(base);
      return build_type(inner);
    } else {
      diags_.error(node_range(base), "Unsupported base_type in this subtask");
      built_base = ast_.create<InferType>(node_range(base));
    }

    const bool nullable = has_child_kind(type_node, "?");
    return ast_.create<TypeExpr>(built_base, nullable, node_range(type_node));
  }

  // If we were given a base_type/primary_type directly.
  if (type_node.kind() == "base_type") {
    if (type_node.named_child_count() == 1) return build_type(type_node.named_child(0));
  }

  // Fallback: treat as a base type and wrap into TypeExpr.
  diags_.error(node_range(type_node), "Expected 'type' node");
  auto * infer = ast_.create<InferType>(node_range(type_node));
  return ast_.create<TypeExpr>(infer, false, node_range(type_node));
}

}  // namespace bt_dsl
