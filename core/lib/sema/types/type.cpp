// bt_dsl/sema/type.cpp - Type context implementation
//
#include "bt_dsl/sema/types/type.hpp"

namespace bt_dsl
{

// ============================================================================
// TypeContext Implementation
// ============================================================================

TypeContext::TypeContext()
{
  // Initialize built-in type singletons
  int8_ = Type{TypeKind::Int8};
  int16_ = Type{TypeKind::Int16};
  int32_ = Type{TypeKind::Int32};
  int64_ = Type{TypeKind::Int64};
  uint8_ = Type{TypeKind::UInt8};
  uint16_ = Type{TypeKind::UInt16};
  uint32_ = Type{TypeKind::UInt32};
  uint64_ = Type{TypeKind::UInt64};
  float32_ = Type{TypeKind::Float32};
  float64_ = Type{TypeKind::Float64};
  bool_ = Type{TypeKind::Bool};
  string_ = Type{TypeKind::String};
  error_ = Type{TypeKind::Error};

  // Inference placeholders
  integer_literal_ = Type{TypeKind::IntegerLiteral};
  float_literal_ = Type{TypeKind::FloatLiteral};
  null_literal_ = Type{TypeKind::NullLiteral};
  unknown_ = Type{TypeKind::Unknown};
}

const Type * TypeContext::get_bounded_string_type(uint64_t max_bytes)
{
  // Search for existing
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::BoundedString && t.size == max_bytes) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::BoundedString};
  new_type.size = max_bytes;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::get_static_array_type(const Type * element_type, uint64_t size)
{
  // Search for existing
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::StaticArray && t.element_type == element_type && t.size == size) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::StaticArray};
  new_type.element_type = element_type;
  new_type.size = size;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::get_bounded_array_type(const Type * element_type, uint64_t max_size)
{
  // Search for existing
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::BoundedArray && t.element_type == element_type && t.size == max_size) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::BoundedArray};
  new_type.element_type = element_type;
  new_type.size = max_size;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::get_dynamic_array_type(const Type * element_type)
{
  // Search for existing
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::DynamicArray && t.element_type == element_type) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::DynamicArray};
  new_type.element_type = element_type;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::get_nullable_type(const Type * base_type)
{
  // Don't double-wrap nullable
  if (base_type->is_nullable()) {
    return base_type;
  }

  // Search for existing
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::Nullable && t.base_type == base_type) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::Nullable};
  new_type.base_type = base_type;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::get_extern_type(std::string_view name, const AstNode * decl)
{
  // Search for existing (by declaration pointer for identity)
  for (const auto & t : composite_types_) {
    if (t.kind == TypeKind::Extern && t.decl == decl) {
      return &t;
    }
  }

  // Create new
  Type new_type{TypeKind::Extern};
  new_type.name = name;
  new_type.decl = decl;
  composite_types_.push_back(new_type);
  return &composite_types_.back();
}

const Type * TypeContext::lookup_builtin(std::string_view name) const
{
  // Integer types
  if (name == "int8") return &int8_;
  if (name == "int16") return &int16_;
  if (name == "int32") return &int32_;
  if (name == "int64") return &int64_;
  if (name == "uint8") return &uint8_;
  if (name == "uint16") return &uint16_;
  if (name == "uint32") return &uint32_;
  if (name == "uint64") return &uint64_;

  // Float types
  if (name == "float32") return &float32_;
  if (name == "float64") return &float64_;

  // Other primitives
  if (name == "bool") return &bool_;
  if (name == "string") return &string_;

  // Aliases (per spec §3.1.4.1)
  if (name == "int") return &int32_;
  if (name == "float") return &float32_;
  if (name == "double") return &float64_;
  if (name == "byte" || name == "char") return &uint8_;

  return nullptr;
}

}  // namespace bt_dsl
