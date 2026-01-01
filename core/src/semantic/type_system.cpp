// bt_dsl/type_system.cpp - Type system implementation
#include "bt_dsl/semantic/type_system.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "bt_dsl/core/symbol_table.hpp"
#include "bt_dsl/semantic/node_registry.hpp"

namespace bt_dsl
{

namespace
{

bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_'; }

bool is_ident_continue(unsigned char c) { return std::isalnum(c) || c == '_'; }

struct TypeParser
{
  std::string_view text;
  size_t pos = 0;

  explicit TypeParser(std::string_view t) : text(t) {}

  [[nodiscard]] bool eof() const { return pos >= text.size(); }

  void skip_ws()
  {
    while (!eof() && std::isspace(static_cast<unsigned char>(text[pos]))) {
      pos++;
    }
  }

  [[nodiscard]] bool consume_char(char c)
  {
    skip_ws();
    if (!eof() && text[pos] == c) {
      pos++;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool consume_str(std::string_view s)
  {
    skip_ws();
    if (text.substr(pos, s.size()) == s) {
      pos += s.size();
      return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string> parse_identifier()
  {
    skip_ws();
    if (eof()) return std::nullopt;
    const auto c0 = static_cast<unsigned char>(text[pos]);
    if (!is_ident_start(c0)) return std::nullopt;
    const size_t start = pos;
    pos++;
    while (!eof() && is_ident_continue(static_cast<unsigned char>(text[pos]))) {
      pos++;
    }
    return std::string(text.substr(start, pos - start));
  }

  [[nodiscard]] std::optional<uint64_t> parse_uint()
  {
    skip_ws();
    if (eof() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
      return std::nullopt;
    }
    uint64_t v = 0;
    while (!eof() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      const auto digit = static_cast<uint64_t>(text[pos] - '0');
      if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
        return std::nullopt;
      }
      v = (v * 10) + digit;
      pos++;
    }
    return v;
  }

  [[nodiscard]] static TypeParseResult error(std::string msg)
  {
    return TypeParseResult{Type::unknown(), std::move(msg)};
  }

  TypeParseResult parse_type(bool require_eof = true)
  {
    auto base = parse_base_type();
    if (base.has_error()) return base;

    // Optional nullable suffix
    skip_ws();
    if (consume_char('?')) {
      // infer_type already encodes nullable; forbid "_??" etc.
      if (const auto * inf = std::get_if<TypeInfer>(&base.type.value)) {
        if (inf->nullable) {
          return error("Invalid type: multiple '?' suffixes");
        }
        TypeInfer updated = *inf;
        updated.nullable = true;
        base.type = Type{updated};
      } else {
        base.type = Type::nullable(std::move(base.type));
      }
    }

    skip_ws();
    if (require_eof) {
      if (!eof()) {
        return error("Unexpected trailing characters in type");
      }
    }
    return base;
  }

  TypeParseResult parse_base_type()
  {
    skip_ws();
    if (eof()) {
      return error("Expected type");
    }

    // infer_type: _ or _?
    // IMPORTANT: '_' is also a valid leading character for identifiers.
    // We only treat it as infer_type when it is a standalone token (optionally
    // followed by '?'). Examples:
    //   _      => infer
    //   _?     => infer nullable
    //   _Foo   => identifier (private name)
    if (!eof() && text[pos] == '_') {
      const char next = (pos + 1 < text.size()) ? text[pos + 1] : '\0';
      const bool next_is_ident_char = (next >= 'a' && next <= 'z') ||
                                      (next >= 'A' && next <= 'Z') ||
                                      (next >= '0' && next <= '9') || (next == '_');
      if (!next_is_ident_char) {
        (void)consume_char('_');
        bool nullable = false;
        skip_ws();
        if (consume_char('?')) {
          nullable = true;
        }
        return TypeParseResult{Type::infer(nullable), std::nullopt};
      }
    }

    // static array: [ type ; (<=)? (int|ident) ]
    if (consume_char('[')) {
      // element type
      TypeParser sub(text.substr(pos));
      auto elem_res = sub.parse_type(/*require_eof=*/false);
      if (elem_res.has_error()) {
        return error(elem_res.error ? *elem_res.error : "Failed to parse array element type");
      }
      pos += sub.pos;

      if (!consume_char(';')) {
        return error("Expected ';' in static array type");
      }

      TypeArraySizeKind kind = TypeArraySizeKind::Exact;
      skip_ws();
      if (consume_str("<=")) {
        kind = TypeArraySizeKind::Max;
      }

      TypeArraySizeExpr size_expr;
      if (auto u = parse_uint()) {
        size_expr.value = *u;
      } else if (auto id = parse_identifier()) {
        size_expr.value = *id;
      } else {
        return error("Expected array size (integer or identifier)");
      }

      if (!consume_char(']')) {
        return error("Expected ']' to close static array type");
      }
      return TypeParseResult{
        Type::static_array(std::move(elem_res.type), kind, std::move(size_expr)), std::nullopt};
    }

    // dynamic array: vec<type>
    {
      const size_t saved = pos;
      if (consume_str("vec")) {
        // ensure keyword boundary
        if (
          saved + 3 < text.size() &&
          is_ident_continue(static_cast<unsigned char>(text[saved + 3]))) {
          pos = saved;
        } else {
          if (!consume_char('<')) {
            return error("Expected '<' after vec");
          }
          TypeParser sub(text.substr(pos));
          auto elem_res = sub.parse_type(/*require_eof=*/false);
          if (elem_res.has_error()) {
            return error(elem_res.error ? *elem_res.error : "Failed to parse vec element type");
          }
          pos += sub.pos;
          if (!consume_char('>')) {
            return error("Expected '>' to close vec<...>");
          }
          return TypeParseResult{Type::vec(std::move(elem_res.type)), std::nullopt};
        }
      }
    }

    // bounded string: string<N> or string<SIZE>
    {
      const size_t saved = pos;
      if (consume_str("string")) {
        if (
          saved + 6 < text.size() &&
          is_ident_continue(static_cast<unsigned char>(text[saved + 6]))) {
          pos = saved;
        } else {
          skip_ws();
          if (consume_char('<')) {
            TypeArraySizeExpr size;
            if (auto u = parse_uint()) {
              size.value = *u;
            } else if (auto id = parse_identifier()) {
              size.value = *id;
            } else {
              return error("Expected array size (integer or identifier) in string<...>");
            }
            if (!consume_char('>')) {
              return error("Expected '>' to close string<...>");
            }
            return TypeParseResult{Type::bounded_string(std::move(size)), std::nullopt};
          }
          // plain string
          return TypeParseResult{Type::string_type(), std::nullopt};
        }
      }
    }

    // primary identifier
    auto id = parse_identifier();
    if (!id) {
      return error("Expected type name");
    }

    // Normalize to lowercase for builtins/builtin-aliases
    std::string lower = *id;
    std::transform(
      lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    // Disallow internal-only types in surface syntax.
    // Reference: docs/reference/type-system/type-definitions.md 3.1.6
    if (lower == "any") {
      return error("Type 'any' is not part of the surface type syntax");
    }
    if (lower == "unknown") {
      return error("Type 'unknown' is not part of the surface type syntax");
    }

    // bool
    if (lower == "bool") return TypeParseResult{Type::bool_type(), std::nullopt};

    // integer aliases
    if (lower == "byte" || lower == "char")
      return TypeParseResult{Type::int_type(false, 8), std::nullopt};
    if (lower == "int") return TypeParseResult{Type::int_type(true, 32), std::nullopt};

    // floats aliases
    if (lower == "float") return TypeParseResult{Type::float_type(32), std::nullopt};
    if (lower == "double") return TypeParseResult{Type::float_type(64), std::nullopt};

    // exact primitive spellings
    const auto parse_int_bits = [&](bool is_signed) -> std::optional<uint8_t> {
      const std::string prefix = is_signed ? "int" : "uint";
      if (lower.rfind(prefix, 0) != 0) return std::nullopt;
      const std::string bits_str = lower.substr(prefix.size());
      if (bits_str == "8") return 8;
      if (bits_str == "16") return 16;
      if (bits_str == "32") return 32;
      if (bits_str == "64") return 64;
      return std::nullopt;
    };

    if (auto b = parse_int_bits(true))
      return TypeParseResult{Type::int_type(true, *b), std::nullopt};
    if (auto b = parse_int_bits(false))
      return TypeParseResult{Type::int_type(false, *b), std::nullopt};

    if (lower == "float32") return TypeParseResult{Type::float_type(32), std::nullopt};
    if (lower == "float64") return TypeParseResult{Type::float_type(64), std::nullopt};

    // unknown identifier: keep as named (resolved later)
    return TypeParseResult{Type::named(*id), std::nullopt};
  }
};

std::optional<int64_t> int_min_for(uint8_t bits)
{
  switch (bits) {
    case 8:
      return std::numeric_limits<int8_t>::min();
    case 16:
      return std::numeric_limits<int16_t>::min();
    case 32:
      return std::numeric_limits<int32_t>::min();
    case 64:
      return std::numeric_limits<int64_t>::min();
    default:
      return std::nullopt;
  }
}

std::optional<int64_t> int_max_for(uint8_t bits)
{
  switch (bits) {
    case 8:
      return std::numeric_limits<int8_t>::max();
    case 16:
      return std::numeric_limits<int16_t>::max();
    case 32:
      return std::numeric_limits<int32_t>::max();
    case 64:
      return std::numeric_limits<int64_t>::max();
    default:
      return std::nullopt;
  }
}

std::optional<uint64_t> uint_max_for(uint8_t bits)
{
  switch (bits) {
    case 8:
      return std::numeric_limits<uint8_t>::max();
    case 16:
      return std::numeric_limits<uint16_t>::max();
    case 32:
      return std::numeric_limits<uint32_t>::max();
    case 64:
      return std::numeric_limits<uint64_t>::max();
    default:
      return std::nullopt;
  }
}

bool checked_add_i64(int64_t a, int64_t b, int64_t & out)
{
  // Avoid non-standard __int128 (this project builds with -Werror=pedantic).
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_add_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();
  if ((b > 0 && a > maxv - b) || (b < 0 && a < minv - b)) {
    return false;
  }
  out = a + b;
  return true;
#endif
}

bool checked_sub_i64(int64_t a, int64_t b, int64_t & out)
{
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_sub_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();
  if ((b > 0 && a < minv + b) || (b < 0 && a > maxv + b)) {
    return false;
  }
  out = a - b;
  return true;
#endif
}

bool checked_mul_i64(int64_t a, int64_t b, int64_t & out)
{
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_mul_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();

  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }

  // Handle the one case where abs() would overflow.
  if ((a == -1 && b == minv) || (b == -1 && a == minv)) {
    return false;
  }

  if (a > 0) {
    if (b > 0) {
      if (a > maxv / b) return false;
    } else {
      if (b < minv / a) return false;
    }
  } else {  // a < 0
    if (b > 0) {
      if (a < minv / b) return false;
    } else {  // b < 0
      if (b < maxv / a) return false;
    }
  }

  out = a * b;
  return true;
#endif
}

bool checked_div_i64(int64_t a, int64_t b, int64_t & out)
{
  if (b == 0) {
    return false;
  }
  // INT64_MIN / -1 overflows.
  if (a == std::numeric_limits<int64_t>::min() && b == -1) {
    return false;
  }
  out = a / b;
  return true;
}

bool checked_mod_i64(int64_t a, int64_t b, int64_t & out)
{
  if (b == 0) {
    return false;
  }
  // same overflow edge case as division
  if (a == std::numeric_limits<int64_t>::min() && b == -1) {
    return false;
  }
  out = a % b;
  return true;
}

}  // namespace

// ============================================================================
// Type parsing / queries / printing
// ============================================================================

TypeParseResult Type::parse(std::string_view text)
{
  TypeParser p(text);
  return p.parse_type(/*require_eof=*/true);
}

Type Type::from_string(std::string_view text)
{
  auto r = Type::parse(text);
  if (r.has_error()) {
    return Type::unknown();
  }
  return std::move(r.type);
}

bool Type::is_integer() const
{
  return std::holds_alternative<TypeInt>(value) ||
         std::holds_alternative<TypeIntegerLiteral>(value);
}

bool Type::is_float() const
{
  return std::holds_alternative<TypeFloat>(value) ||
         std::holds_alternative<TypeFloatLiteral>(value);
}

bool Type::is_numeric() const
{
  if (is_integer() || is_float()) return true;
  if (const auto * n = std::get_if<TypeNullable>(&value)) {
    return n->base->is_numeric();
  }
  return false;
}

bool Type::is_nullable() const
{
  if (std::holds_alternative<TypeNullable>(value)) return true;
  if (const auto * inf = std::get_if<TypeInfer>(&value)) return inf->nullable;
  return false;
}

bool Type::equals(const Type & other) const
{
  if (value.index() != other.value.index()) {
    return false;
  }

  return std::visit(
    [&](const auto & a) -> bool {
      using A = std::decay_t<decltype(a)>;
      const A * b = std::get_if<A>(&other.value);
      if (!b) return false;

      if constexpr (std::is_same_v<A, TypeInt>) {
        return a.is_signed == b->is_signed && a.bits == b->bits;
      } else if constexpr (std::is_same_v<A, TypeFloat>) {
        return a.bits == b->bits;
      } else if constexpr (
        std::is_same_v<A, TypeBool> || std::is_same_v<A, TypeString> ||
        std::is_same_v<A, TypeNullLiteral> || std::is_same_v<A, TypeAny> ||
        std::is_same_v<A, TypeUnknown>) {
        return true;
      } else if constexpr (std::is_same_v<A, TypeBoundedString>) {
        return a.max_bytes.value == b->max_bytes.value;
      } else if constexpr (std::is_same_v<A, TypeStaticArray>) {
        return a.size_kind == b->size_kind && a.size.value == b->size.value &&
               a.element->equals(*b->element);
      } else if constexpr (std::is_same_v<A, TypeVec>) {
        return a.element->equals(*b->element);
      } else if constexpr (std::is_same_v<A, TypeNullable>) {
        return a.base->equals(*b->base);
      } else if constexpr (std::is_same_v<A, TypeInfer>) {
        return a.nullable == b->nullable && a.is_type_var == b->is_type_var;
      } else if constexpr (std::is_same_v<A, TypeNamed> || std::is_same_v<A, TypeExtern>) {
        return a.name == b->name;
      } else if constexpr (
        std::is_same_v<A, TypeIntegerLiteral> || std::is_same_v<A, TypeFloatLiteral>) {
        return a.value == b->value;
      } else if constexpr (std::is_same_v<A, TypeStringLiteral>) {
        return a.byte_len == b->byte_len;
      } else {
        return false;
      }
    },
    value);
}

std::string Type::to_string() const
{
  return std::visit(
    [&](const auto & v) -> std::string {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, TypeInt>) {
        return std::string(v.is_signed ? "int" : "uint") + std::to_string(v.bits);
      } else if constexpr (std::is_same_v<T, TypeFloat>) {
        return std::string("float") + std::to_string(v.bits);
      } else if constexpr (std::is_same_v<T, TypeBool>) {
        return "bool";
      } else if constexpr (std::is_same_v<T, TypeString> || std::is_same_v<T, TypeStringLiteral>) {
        return "string";
      } else if constexpr (std::is_same_v<T, TypeBoundedString>) {
        std::string out = "string<";
        if (const auto * lit = std::get_if<uint64_t>(&v.max_bytes.value)) {
          out += std::to_string(*lit);
        } else {
          out += std::get<std::string>(v.max_bytes.value);
        }
        out += ">";
        return out;
      } else if constexpr (std::is_same_v<T, TypeStaticArray>) {
        std::string out = "[" + v.element->to_string() + "; ";
        if (v.size_kind == TypeArraySizeKind::Max) {
          out += "<=";
        }
        if (const auto * lit = std::get_if<uint64_t>(&v.size.value)) {
          out += std::to_string(*lit);
        } else {
          out += std::get<std::string>(v.size.value);
        }
        out += "]";
        return out;
      } else if constexpr (std::is_same_v<T, TypeVec>) {
        return "vec<" + v.element->to_string() + ">";
      } else if constexpr (std::is_same_v<T, TypeNullable>) {
        return v.base->to_string() + "?";
      } else if constexpr (std::is_same_v<T, TypeInfer>) {
        // Keep diagnostics stable: print syntax wildcard-like forms.
        // Internal type variables are not part of the surface syntax.
        if (v.nullable) {
          return "_?";
        }
        return "_";
      } else if constexpr (std::is_same_v<T, TypeNamed> || std::is_same_v<T, TypeExtern>) {
        return v.name;
      } else if constexpr (std::is_same_v<T, TypeIntegerLiteral>) {
        return "{integer}";
      } else if constexpr (std::is_same_v<T, TypeFloatLiteral>) {
        return "{float}";
      } else if constexpr (std::is_same_v<T, TypeNullLiteral>) {
        return "null";
      } else if constexpr (std::is_same_v<T, TypeAny>) {
        return "any";
      } else {
        return "unknown";
      }
    },
    value);
}

bool Type::is_compatible_with(const Type & other) const
{
  // NOTE: this method is used as "target.is_compatible_with(source)".

  const Type & source = other;

  if (is_any() || source.is_any()) {
    return true;
  }
  if (is_unknown() || source.is_unknown()) {
    // keep best-effort behavior to avoid cascading errors
    return true;
  }

  // null literal
  if (std::holds_alternative<TypeNullLiteral>(source.value)) {
    return is_nullable();
  }

  // nullable target
  if (const auto * tgt_n = std::get_if<TypeNullable>(&value)) {
    // null already handled above
    if (const auto * src_n = std::get_if<TypeNullable>(&source.value)) {
      return tgt_n->base->is_compatible_with(*src_n->base);
    }
    return tgt_n->base->is_compatible_with(source);
  }

  // non-nullable target cannot accept nullable source
  if (std::holds_alternative<TypeNullable>(source.value)) {
    return false;
  }

  // exact equality
  if (equals(source)) {
    return true;
  }

  // extern types: only identical names
  if (const auto * a = std::get_if<TypeExtern>(&value)) {
    if (const auto * b = std::get_if<TypeExtern>(&source.value)) {
      return a->name == b->name;
    }
    return false;
  }

  // bounded strings
  if (std::holds_alternative<TypeString>(value)) {
    // bounded -> string is OK
    if (std::holds_alternative<TypeBoundedString>(source.value)) return true;
    if (std::holds_alternative<TypeStringLiteral>(source.value)) return true;
  }

  if (const auto * tgt_bs = std::get_if<TypeBoundedString>(&value)) {
    if (const auto * src_bs = std::get_if<TypeBoundedString>(&source.value)) {
      const auto * t_lit = std::get_if<uint64_t>(&tgt_bs->max_bytes.value);
      const auto * s_lit = std::get_if<uint64_t>(&src_bs->max_bytes.value);
      if (t_lit && s_lit) {
        return *s_lit <= *t_lit;
      }
      // If we cannot compare bounds, require exact textual equality.
      return tgt_bs->max_bytes.value == src_bs->max_bytes.value;
    }
    if (const auto * src_sl = std::get_if<TypeStringLiteral>(&source.value)) {
      const auto * t_lit = std::get_if<uint64_t>(&tgt_bs->max_bytes.value);
      if (!t_lit) {
        return false;
      }
      return src_sl->byte_len <= *t_lit;
    }
    // string -> bounded is forbidden
    return false;
  }

  // numeric widening
  if (const auto * tgt_i = std::get_if<TypeInt>(&value)) {
    // literal integers may fit
    if (const auto * lit = std::get_if<TypeIntegerLiteral>(&source.value)) {
      if (tgt_i->is_signed) {
        const auto lo = int_min_for(tgt_i->bits);
        const auto hi = int_max_for(tgt_i->bits);
        if (!lo || !hi) return false;
        return (*lo <= lit->value) && (lit->value <= *hi);
      }
      if (lit->value < 0) return false;
      const auto hiu = uint_max_for(tgt_i->bits);
      if (!hiu) return false;
      return static_cast<uint64_t>(lit->value) <= *hiu;
    }

    if (const auto * src_i = std::get_if<TypeInt>(&source.value)) {
      if (tgt_i->is_signed != src_i->is_signed) {
        return false;
      }
      return src_i->bits <= tgt_i->bits;
    }
    // no implicit float<->int
    return false;
  }

  if (const auto * tgt_f = std::get_if<TypeFloat>(&value)) {
    if (const auto * src_f = std::get_if<TypeFloat>(&source.value)) {
      return src_f->bits <= tgt_f->bits;
    }
    if (std::holds_alternative<TypeFloatLiteral>(source.value)) {
      // best-effort: accept; range/precision checks can be added later
      return true;
    }
    return false;
  }

  // static arrays
  if (const auto * tgt_arr = std::get_if<TypeStaticArray>(&value)) {
    const auto * src_arr = std::get_if<TypeStaticArray>(&source.value);
    if (!src_arr) return false;
    if (!tgt_arr->element->equals(*src_arr->element)) return false;

    // if both size expressions are literal, we can compare
    const auto * tgt_lit = std::get_if<uint64_t>(&tgt_arr->size.value);
    const auto * src_lit = std::get_if<uint64_t>(&src_arr->size.value);
    if (
      tgt_arr->size_kind == TypeArraySizeKind::Exact &&
      src_arr->size_kind == TypeArraySizeKind::Exact) {
      if (tgt_lit && src_lit) return *tgt_lit == *src_lit;
      // otherwise require textual equality
      return tgt_arr->size.value == src_arr->size.value;
    }

    // target bounded
    if (tgt_arr->size_kind == TypeArraySizeKind::Max) {
      // src exact: OK if N <= M
      if (src_arr->size_kind == TypeArraySizeKind::Exact && tgt_lit && src_lit) {
        return *src_lit <= *tgt_lit;
      }
      // src bounded: OK if N <= M
      if (src_arr->size_kind == TypeArraySizeKind::Max && tgt_lit && src_lit) {
        return *src_lit <= *tgt_lit;
      }
      // cannot compare symbolic bounds here
      return false;
    }

    // target exact, src bounded is forbidden
    return false;
  }

  // vec
  if (const auto * tgt_vec = std::get_if<TypeVec>(&value)) {
    const auto * src_vec = std::get_if<TypeVec>(&source.value);
    if (!src_vec) return false;
    return tgt_vec->element->equals(*src_vec->element);
  }

  // named types (unresolved): only exact
  if (const auto * a = std::get_if<TypeNamed>(&value)) {
    if (const auto * b = std::get_if<TypeNamed>(&source.value)) {
      return a->name == b->name;
    }
    return false;
  }

  return false;
}

// ============================================================================
// TypeEnvironment
// ============================================================================

void TypeEnvironment::add_extern_type(std::string name) { extern_types_.insert(std::move(name)); }

void TypeEnvironment::add_type_alias(std::string name, Type type)
{
  aliases_.insert_or_assign(std::move(name), std::move(type));
}

bool TypeEnvironment::is_extern_type(std::string_view name) const
{
  return extern_types_.count(std::string(name)) > 0;
}

bool TypeEnvironment::has_alias(std::string_view name) const
{
  return aliases_.find(std::string(name)) != aliases_.end();
}

Type TypeEnvironment::resolve(const Type & t, std::optional<std::string> * error) const
{
  std::unordered_set<std::string> visiting;
  return resolve_impl(t, visiting, error);
}

Type TypeEnvironment::resolve_impl(
  const Type & t, std::unordered_set<std::string> & visiting,
  std::optional<std::string> * error) const
{
  return std::visit(
    [&](const auto & v) -> Type {
      using T = std::decay_t<decltype(v)>;

      if constexpr (std::is_same_v<T, TypeNamed>) {
        // extern types
        if (is_extern_type(v.name)) {
          return Type::extern_type(v.name);
        }

        // aliases
        auto it = aliases_.find(v.name);
        if (it != aliases_.end()) {
          if (visiting.count(v.name)) {
            if (error) *error = "Cyclic type alias: " + v.name;
            return Type::unknown();
          }
          visiting.insert(v.name);
          Type expanded = resolve_impl(it->second, visiting, error);
          visiting.erase(v.name);
          return expanded;
        }

        if (error) *error = "Unknown type: " + v.name;
        return Type::unknown();
      } else if constexpr (std::is_same_v<T, TypeNullable>) {
        Type base = resolve_impl(*v.base, visiting, error);
        if (base.is_unknown()) return Type::unknown();
        return Type::nullable(std::move(base));
      } else if constexpr (std::is_same_v<T, TypeVec>) {
        Type elem = resolve_impl(*v.element, visiting, error);
        if (elem.is_unknown()) return Type::unknown();
        return Type::vec(std::move(elem));
      } else if constexpr (std::is_same_v<T, TypeStaticArray>) {
        Type elem = resolve_impl(*v.element, visiting, error);
        if (elem.is_unknown()) return Type::unknown();
        TypeStaticArray arr;
        arr.element = Box<Type>(std::move(elem));
        arr.size_kind = v.size_kind;
        arr.size = v.size;
        return Type{std::move(arr)};
      } else {
        return t;
      }
    },
    t.value);
}

// ============================================================================
// TypeContext Implementation
// ============================================================================

void TypeContext::set_type(std::string_view name, Type type)
{
  types_.insert_or_assign(std::string(name), std::move(type));
}

const Type * TypeContext::get_type(std::string_view name) const
{
  const std::string key(name);
  auto it = types_.find(key);
  if (it != types_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool TypeContext::has_type(std::string_view name) const { return get_type(name) != nullptr; }

// ============================================================================
// TypeInferenceResult Implementation
// ============================================================================

TypeInferenceResult TypeInferenceResult::success(Type t)
{
  return TypeInferenceResult{std::move(t), std::nullopt};
}

TypeInferenceResult TypeInferenceResult::failure(Type t, std::string error_message)
{
  return TypeInferenceResult{std::move(t), std::move(error_message)};
}

// ============================================================================
// TypeResolver Implementation
// ============================================================================

TypeResolver::TypeResolver(
  const SymbolTable & symbols, const NodeRegistry & nodes, const TypeEnvironment * env)
: symbols_(symbols), nodes_(nodes), env_(env)
{
}

namespace
{

// Const-evaluate an integer constant expression.
// This is intentionally limited to what the reference allows for const_expr and
// what we need for repeat-init sizes: integer literals, const references, unary
// '-', and integer binary ops.
// Reference: docs/reference/declarations-and-scopes.md 4.3, and
// docs/reference/type-system/inference-and-resolution.md (array repeat-init).
struct SimpleConstEval
{
  const SymbolTable & symbols;
  const Scope * scope;
  const TypeEnvironment * env;
  std::unordered_map<std::string, std::optional<int64_t>> memo;
  std::unordered_set<std::string> in_stack;

  std::optional<int64_t> eval(const Expression & expr)
  {
    return std::visit(
      [&](const auto & e) -> std::optional<int64_t> {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, Literal>) {
          if (const auto * i = std::get_if<IntLiteral>(&e)) {
            return i->value;
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, VarRef>) {
          if (e.direction.has_value()) {
            return std::nullopt;
          }
          const Symbol * sym = symbols.resolve(e.name, scope);
          if (!sym || !sym->is_const() || sym->kind == SymbolKind::Parameter) {
            return std::nullopt;
          }
          // Memoized by symbol name (works for globals and locals).
          if (auto it = memo.find(sym->name); it != memo.end()) {
            return it->second;
          }
          if (in_stack.count(sym->name)) {
            memo[sym->name] = std::nullopt;
            return std::nullopt;
          }
          if (!sym->ast_node) {
            memo[sym->name] = std::nullopt;
            return std::nullopt;
          }

          const auto * decl = static_cast<const ConstDeclStmt *>(sym->ast_node);
          in_stack.insert(sym->name);
          auto v = eval(decl->value);
          in_stack.erase(sym->name);
          memo[sym->name] = v;
          return v;
        } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
          auto v = eval(e->operand);
          if (!v) return std::nullopt;
          if (e->op != UnaryOp::Neg) {
            return std::nullopt;
          }
          if (*v == std::numeric_limits<int64_t>::min()) {
            return std::nullopt;
          }
          return -(*v);
        } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
          auto lv = eval(e->left);
          auto rv = eval(e->right);
          if (!lv || !rv) return std::nullopt;

          int64_t out = 0;
          switch (e->op) {
            case BinaryOp::Add:
              if (!checked_add_i64(*lv, *rv, out)) return std::nullopt;
              return out;
            case BinaryOp::Sub:
              if (!checked_sub_i64(*lv, *rv, out)) return std::nullopt;
              return out;
            case BinaryOp::Mul:
              if (!checked_mul_i64(*lv, *rv, out)) return std::nullopt;
              return out;
            case BinaryOp::Div:
              if (!checked_div_i64(*lv, *rv, out)) return std::nullopt;
              return out;
            case BinaryOp::Mod:
              if (!checked_mod_i64(*lv, *rv, out)) return std::nullopt;
              return out;
            case BinaryOp::BitAnd:
              return (*lv) & (*rv);
            case BinaryOp::BitOr:
              return (*lv) | (*rv);
            default:
              return std::nullopt;
          }
        }

        // Integer casts inside const_expr (needed e.g. for array bounds checks).
        if constexpr (std::is_same_v<T, Box<CastExpr>>) {
          auto v = eval(e->expr);
          if (!v) return std::nullopt;

          TypeParseResult parsed = Type::parse(e->type_name);
          if (parsed.has_error()) {
            return std::nullopt;
          }

          Type tgt = std::move(parsed.type);
          if (env) {
            tgt = env->resolve(tgt);
          }

          // Allow nullable wrappers by evaluating against the base type.
          if (const auto * n = std::get_if<TypeNullable>(&tgt.value)) {
            tgt = *n->base;
          }

          if (const auto * ti = std::get_if<TypeInt>(&tgt.value)) {
            const int64_t val = *v;
            if (ti->is_signed) {
              const auto lo = int_min_for(ti->bits);
              const auto hi = int_max_for(ti->bits);
              if (!lo || !hi) {
                return std::nullopt;
              }
              if (val < *lo || val > *hi) {
                return std::nullopt;
              }
              return val;
            }

            if (val < 0) {
              return std::nullopt;
            }
            const auto hi = uint_max_for(ti->bits);
            if (!hi) {
              return std::nullopt;
            }
            if (static_cast<uint64_t>(val) > *hi) {
              return std::nullopt;
            }
            return val;
          }

          return std::nullopt;
        }

        // Not an integer const_expr we can use for sizes.
        return std::nullopt;
      },
      expr);
  }
};

struct NumericCommonTypeResult
{
  Type type;
  std::optional<std::string> error;
};

bool is_float_lit(const Type & t) { return std::holds_alternative<TypeFloatLiteral>(t.value); }

std::optional<uint8_t> minimal_signed_bits_for_value(int64_t v)
{
  for (uint8_t bits : {uint8_t(8), uint8_t(16), uint8_t(32), uint8_t(64)}) {
    const auto lo = int_min_for(bits);
    const auto hi = int_max_for(bits);
    if (lo && hi && (*lo <= v) && (v <= *hi)) {
      return bits;
    }
  }
  return std::nullopt;
}

std::optional<uint8_t> minimal_unsigned_bits_for_value(uint64_t v)
{
  for (uint8_t bits : {uint8_t(8), uint8_t(16), uint8_t(32), uint8_t(64)}) {
    const auto hi = uint_max_for(bits);
    if (hi && v <= *hi) {
      return bits;
    }
  }
  return std::nullopt;
}

// Compute the minimal common type reachable via widening that can represent both.
// Reference: docs/reference/type-system/expression-typing.md 3.4.2 and
// docs/reference/type-system/compatibility-and-conversion.md 3.3.3.
NumericCommonTypeResult common_numeric_type(const Type & a, const Type & b)
{
  // Disallow nullable numeric in arithmetic/ordering contexts (caller may handle).

  // No implicit int <-> float in widening table.
  const bool a_int = a.is_integer();
  const bool b_int = b.is_integer();
  const bool a_f = a.is_float();
  const bool b_f = b.is_float();
  if ((a_int && b_f) || (a_f && b_int)) {
    return {Type::unknown(), "Integer/float operands cannot be mixed without an explicit cast"};
  }

  // Float domain.
  if (a_f && b_f) {
    uint8_t bits = 32;
    if (const auto * fa = std::get_if<TypeFloat>(&a.value)) bits = std::max(bits, fa->bits);
    if (const auto * fb = std::get_if<TypeFloat>(&b.value)) bits = std::max(bits, fb->bits);

    // If either side is a float literal, only choose float32 when it is in-range.
    const auto fits_f32 = [](double v) {
      if (!std::isfinite(v)) return false;
      const auto maxf = static_cast<double>(std::numeric_limits<float>::max());
      return std::fabs(v) <= maxf;
    };

    if (is_float_lit(a) || is_float_lit(b)) {
      if (bits == 32) {
        const auto * la = std::get_if<TypeFloatLiteral>(&a.value);
        const auto * lb = std::get_if<TypeFloatLiteral>(&b.value);
        if ((la && !fits_f32(la->value)) || (lb && !fits_f32(lb->value))) {
          bits = 64;
        }
      }
      // If any concrete side is float64, widening forces float64.
      if (bits == 64) {
        return {Type::float_type(64), std::nullopt};
      }
      return {Type::float_type(32), std::nullopt};
    }

    return {Type::float_type(bits == 64 ? 64 : 32), std::nullopt};
  }

  // Integer domain.
  if (a_int && b_int) {
    // Determine desired signedness.
    const auto * ai = std::get_if<TypeInt>(&a.value);
    const auto * bi = std::get_if<TypeInt>(&b.value);

    // Concrete int vs concrete int: signedness must match.
    if (ai && bi && ai->is_signed != bi->is_signed) {
      return {Type::unknown(), "Signed/unsigned integer operands cannot be mixed"};
    }

    // If one side is concrete, use its signedness as the target signedness.
    std::optional<bool> target_signed;
    if (ai) target_signed = ai->is_signed;
    if (bi) target_signed = bi->is_signed;

    // If both are literals, keep literal typing (caller may constant-fold separately).
    if (!ai && !bi) {
      return {Type::int_type(true, 32), std::nullopt};
    }

    const auto target_is_signed = target_signed.value_or(true);
    uint8_t bits = 8;
    if (ai) bits = std::max(bits, ai->bits);
    if (bi) bits = std::max(bits, bi->bits);

    // Constrain by literal values, choosing the minimal bit-width that still
    // represents the literal and is >= the non-literal operand width.
    const auto apply_int_lit = [&](const Type & t) -> std::optional<std::string> {
      if (const auto * lit = std::get_if<TypeIntegerLiteral>(&t.value)) {
        const int64_t v = lit->value;
        if (!target_is_signed && v < 0) {
          return "Unsigned integer operands cannot be negative";
        }
        if (target_is_signed) {
          auto mb = minimal_signed_bits_for_value(v);
          if (!mb) return "Integer literal is out of representable range";
          bits = std::max(bits, *mb);
        } else {
          auto mb = minimal_unsigned_bits_for_value(static_cast<uint64_t>(v));
          if (!mb) return "Integer literal is out of representable range";
          bits = std::max(bits, *mb);
        }
      }
      return std::nullopt;
    };

    if (auto err = apply_int_lit(a)) return {Type::unknown(), std::move(err)};
    if (auto err = apply_int_lit(b)) return {Type::unknown(), std::move(err)};

    // Clamp to supported widths.
    if (bits <= 8)
      bits = 8;
    else if (bits <= 16)
      bits = 16;
    else if (bits <= 32)
      bits = 32;
    else
      bits = 64;

    return {Type::int_type(target_is_signed, bits), std::nullopt};
  }

  return {Type::unknown(), "Operator cannot be applied to non-numeric types"};
}

}  // namespace

namespace
{

// Convert internal literal types to their default concrete types when they
// become the declared type of a variable/const.
// Reference: docs/reference/type-system.md (literal default resolution).
Type finalize_default_type(Type t)
{
  // Apply defaulting recursively to composite types.
  if (auto * v = std::get_if<TypeVec>(&t.value)) {
    Type elem = finalize_default_type(*v->element);
    return Type::vec(std::move(elem));
  }
  if (auto * a = std::get_if<TypeStaticArray>(&t.value)) {
    Type elem = finalize_default_type(*a->element);
    return Type::static_array(std::move(elem), a->size_kind, a->size);
  }
  if (auto * n = std::get_if<TypeNullable>(&t.value)) {
    Type base = finalize_default_type(*n->base);
    return Type::nullable(std::move(base));
  }

  // NOTE: Defaulting is a scope-end operation in the reference spec.
  // This helper is only used for places where the implementation historically
  // required early concretization; keep it conservative.
  if (std::holds_alternative<TypeNullLiteral>(t.value)) {
    // null-initialized variables start as an unresolved nullable type variable.
    return Type::type_var(/*nullable_requirement=*/true);
  }
  return t;
}

bool is_cast_allowed_impl(const Type & src, const Type & dst)
{
  // Be permissive around error-recovery types.
  if (src.is_any() || dst.is_any() || src.is_unknown() || dst.is_unknown()) {
    return true;
  }

  // extern types: only identical names; no other casts allowed (even explicit)
  const auto * se = std::get_if<TypeExtern>(&src.value);
  const auto * de = std::get_if<TypeExtern>(&dst.value);
  if (se || de) {
    return (se && de && se->name == de->name);
  }

  // null literal
  if (std::holds_alternative<TypeNullLiteral>(src.value)) {
    return dst.is_nullable();
  }

  // nullable destination
  if (const auto * dn = std::get_if<TypeNullable>(&dst.value)) {
    if (const auto * sn = std::get_if<TypeNullable>(&src.value)) {
      return is_cast_allowed_impl(*sn->base, *dn->base);
    }
    return is_cast_allowed_impl(src, *dn->base);
  }

  // disallow stripping nullability via cast (for now)
  if (std::holds_alternative<TypeNullable>(src.value)) {
    return false;
  }

  // vec casts
  if (const auto * dv = std::get_if<TypeVec>(&dst.value)) {
    if (const auto * sv = std::get_if<TypeVec>(&src.value)) {
      return dv->element->equals(*sv->element);
    }
    if (const auto * sa = std::get_if<TypeStaticArray>(&src.value)) {
      return dv->element->equals(*sa->element);
    }
    return false;
  }

  // static arrays: keep strict (explicit casts between different sizes/types are not specified)
  if (std::holds_alternative<TypeStaticArray>(dst.value)) {
    return dst.equals(src);
  }

  // numeric casts are allowed (explicit)
  if (src.is_numeric() && dst.is_numeric()) {
    return true;
  }

  // strings
  if (std::holds_alternative<TypeString>(dst.value)) {
    return std::holds_alternative<TypeString>(src.value) ||
           std::holds_alternative<TypeStringLiteral>(src.value) ||
           std::holds_alternative<TypeBoundedString>(src.value);
  }
  if (const auto * dbs = std::get_if<TypeBoundedString>(&dst.value)) {
    if (const auto * sbs = std::get_if<TypeBoundedString>(&src.value)) {
      const auto * d_lit = std::get_if<uint64_t>(&dbs->max_bytes.value);
      const auto * s_lit = std::get_if<uint64_t>(&sbs->max_bytes.value);
      if (d_lit && s_lit) {
        return *s_lit <= *d_lit;
      }
      return dbs->max_bytes.value == sbs->max_bytes.value;
    }
    if (const auto * ssl = std::get_if<TypeStringLiteral>(&src.value)) {
      const auto * d_lit = std::get_if<uint64_t>(&dbs->max_bytes.value);
      if (!d_lit) {
        return false;
      }
      return ssl->byte_len <= *d_lit;
    }
    return false;
  }

  // bool
  if (std::holds_alternative<TypeBool>(dst.value)) {
    return std::holds_alternative<TypeBool>(src.value);
  }

  // fallback: only identity
  return dst.equals(src);
}

}  // namespace

TypeContext TypeResolver::resolve_tree_types(const TreeDef & tree)
{
  const Scope * saved_scope = current_scope_;
  current_scope_ = symbols_.tree_scope(tree.name);

  TypeContext ctx;

  const auto wrap_nullable_if_requested = [](Type base, bool requested_nullable) -> Type {
    if (requested_nullable && !base.is_nullable()) {
      return Type::nullable(std::move(base));
    }
    return base;
  };

  const auto resolve_annotation_from_initializer =
    [&](Type annotated, const Expression & init) -> Type {
    // Only try to resolve when the annotation contains inference wildcard(s).
    // This is a minimal implementation (enough for common vec<_>/[_;N]/_? cases).

    auto get_global = [](std::string_view) -> const Type * { return nullptr; };
    auto init_res = infer_expression_type(init, ctx, get_global);
    Type init_t = std::move(init_res.type);
    if (env_) init_t = env_->resolve(init_t);

    // Top-level '_' / '_?'
    if (const auto * infer = std::get_if<TypeInfer>(&annotated.value)) {
      return wrap_nullable_if_requested(std::move(init_t), infer->nullable);
    }

    // vec<_> / vec<_?>
    if (auto * v = std::get_if<TypeVec>(&annotated.value)) {
      if (const auto * infer_elem = std::get_if<TypeInfer>(&v->element->value)) {
        // Infer element from initializer type (vec<T> or [T; N]).
        if (const auto * init_vec = std::get_if<TypeVec>(&init_t.value)) {
          Type elem_t = *init_vec->element;
          elem_t = wrap_nullable_if_requested(std::move(elem_t), infer_elem->nullable);
          return Type::vec(std::move(elem_t));
        }
        if (const auto * init_arr = std::get_if<TypeStaticArray>(&init_t.value)) {
          Type elem_t = *init_arr->element;
          elem_t = wrap_nullable_if_requested(std::move(elem_t), infer_elem->nullable);
          return Type::vec(std::move(elem_t));
        }
      }
    }

    // [_; N] / [_?; N]
    if (auto * a = std::get_if<TypeStaticArray>(&annotated.value)) {
      if (const auto * infer_elem = std::get_if<TypeInfer>(&a->element->value)) {
        if (const auto * init_arr = std::get_if<TypeStaticArray>(&init_t.value)) {
          Type elem_t = *init_arr->element;
          elem_t = wrap_nullable_if_requested(std::move(elem_t), infer_elem->nullable);
          return Type::static_array(std::move(elem_t), a->size_kind, a->size);
        }
      }
    }

    return annotated;
  };

  // 1. Add explicit types from parameters
  for (const auto & param : tree.params) {
    Type t = Type::from_string(param.type_name);
    if (env_) {
      t = env_->resolve(t);
    }
    ctx.set_type(param.name, std::move(t));
  }

  const auto infer_from_block = [&](const std::vector<Statement> & block, auto & self_ref) -> void {
    for (const auto & stmt : block) {
      std::visit(
        [&](const auto & elem) {
          using T = std::decay_t<decltype(elem)>;
          if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
            infer_from_node_usage(*elem, ctx);
          } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
            if (elem.type_name) {
              Type t = Type::from_string(*elem.type_name);
              if (env_) t = env_->resolve(t);

              // Minimal wildcard resolution for annotations: var x: _ = <expr>
              // and var x: _? = <expr>.
              // Reference: docs/reference/type-system/inference-and-resolution.md
              if (elem.initial_value) {
                t = resolve_annotation_from_initializer(std::move(t), *elem.initial_value);
              }

              ctx.set_type(elem.name, std::move(t));
              return;
            }
            if (elem.initial_value) {
              auto get_global = [](std::string_view) -> const Type * { return nullptr; };
              auto result = infer_expression_type(*elem.initial_value, ctx, get_global);
              Type t = finalize_default_type(std::move(result.type));
              if (env_) t = env_->resolve(t);
              ctx.set_type(elem.name, std::move(t));
            } else {
              // Reference: 3.2.4 (local var may start as an unresolved type variable)
              ctx.set_type(elem.name, Type::type_var(/*nullable_requirement=*/false));
            }
          } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
            if (elem.type_name) {
              Type t = Type::from_string(*elem.type_name);
              if (env_) t = env_->resolve(t);

              t = resolve_annotation_from_initializer(std::move(t), elem.value);
              ctx.set_type(elem.name, std::move(t));
              return;
            }
            auto get_global = [](std::string_view) -> const Type * { return nullptr; };
            auto result = infer_expression_type(elem.value, ctx, get_global);
            Type t = std::move(result.type);
            if (env_) t = env_->resolve(t);
            ctx.set_type(elem.name, std::move(t));
          } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
            // Best-effort inference for assignment targets.
            if (!ctx.has_type(elem.target)) {
              auto get_global = [](std::string_view) -> const Type * { return nullptr; };
              auto result = infer_expression_type(elem.value, ctx, get_global);
              Type t = finalize_default_type(std::move(result.type));
              if (env_) t = env_->resolve(t);
              ctx.set_type(elem.target, std::move(t));
            }
          }
        },
        stmt);
    }
    (void)self_ref;
  };

  // 2. Add types from local declarations / consts and seed remaining vars from decls.
  infer_from_block(tree.body, infer_from_block);

  // 3. Constraint propagation pass: refine unresolved type variables (`?`) and
  //    null-derived nullable type variables (`_?`) from assignments and node
  //    port usage.
  // Reference: docs/reference/type-system/inference-and-resolution.md 3.2.3.
  const auto is_unresolved_type_var = [](const Type & t) -> bool {
    if (const auto * inf = std::get_if<TypeInfer>(&t.value)) {
      return inf->is_type_var;
    }
    return false;
  };

  const auto apply_nullable_requirement = [](Type concrete, const Type & var) -> Type {
    const auto * inf = std::get_if<TypeInfer>(&var.value);
    if (inf && inf->nullable && !concrete.is_nullable()) {
      return Type::nullable(std::move(concrete));
    }
    return concrete;
  };

  std::function<bool(const Expression &, const Type &)> try_constrain_expr;
  try_constrain_expr = [&](const Expression & expr, const Type & expected) -> bool {
    // Only handle the cases that directly enable reference-required inference:
    // - VarRef refinement
    // - (x == null) / (x != null) constraints
    // - conjunction
    if (const auto * vr = std::get_if<VarRef>(&expr)) {
      if (const Type * cur = ctx.get_type(vr->name)) {
        if (is_unresolved_type_var(*cur)) {
          Type new_t = apply_nullable_requirement(expected, *cur);
          ctx.set_type(vr->name, std::move(new_t));
          return true;
        }
      }
      return false;
    }

    if (const auto * b = std::get_if<Box<BinaryExpr>>(&expr)) {
      if ((*b)->op == BinaryOp::And) {
        bool changed = false;
        changed |= try_constrain_expr((*b)->left, expected);
        changed |= try_constrain_expr((*b)->right, expected);
        return changed;
      }

      // null comparison: constrain the non-null operand to be nullable.
      if ((*b)->op == BinaryOp::Eq || (*b)->op == BinaryOp::Ne) {
        const auto * l_lit = std::get_if<Literal>(&(*b)->left);
        const auto * r_lit = std::get_if<Literal>(&(*b)->right);
        const bool l_null = l_lit && std::holds_alternative<NullLiteral>(*l_lit);
        const bool r_null = r_lit && std::holds_alternative<NullLiteral>(*r_lit);
        if (l_null) {
          // right must be nullable
          if (const auto * r_vr = std::get_if<VarRef>(&(*b)->right)) {
            if (const Type * cur = ctx.get_type(r_vr->name)) {
              if (is_unresolved_type_var(*cur) && !cur->is_nullable()) {
                TypeInfer v = std::get<TypeInfer>(cur->value);
                v.nullable = true;
                ctx.set_type(r_vr->name, Type{v});
                return true;
              }
            }
          }
        }
        if (r_null) {
          if (const auto * l_vr = std::get_if<VarRef>(&(*b)->left)) {
            if (const Type * cur = ctx.get_type(l_vr->name)) {
              if (is_unresolved_type_var(*cur) && !cur->is_nullable()) {
                TypeInfer v = std::get<TypeInfer>(cur->value);
                v.nullable = true;
                ctx.set_type(l_vr->name, Type{v});
                return true;
              }
            }
          }
        }
      }
    }

    return false;
  };

  const auto default_literals_in_ctx = [&]() {
    // Apply defaulting for remaining literal types in the context.
    // Reference: 3.2.3 (Defaulting).
    std::vector<std::pair<std::string, Type>> updates;
    updates.reserve(ctx.all_types().size());

    for (const auto & kv : ctx.all_types()) {
      const std::string & name = kv.first;
      const Type & t = kv.second;

      auto defaulted = std::visit(
        [&](const auto & v) -> std::optional<Type> {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, TypeIntegerLiteral>) {
            return Type::int_type(true, 32);
          } else if constexpr (std::is_same_v<V, TypeFloatLiteral>) {
            return Type::float_type(64);
          } else if constexpr (std::is_same_v<V, TypeStringLiteral>) {
            return Type::string_type();
          }
          return std::nullopt;
        },
        t.value);

      if (defaulted.has_value()) {
        updates.emplace_back(name, std::move(*defaulted));
      }
    }

    for (auto & u : updates) {
      ctx.set_type(u.first, std::move(u.second));
    }
  };

  // A limited, bounded fixed-point iteration.
  for (int iter = 0; iter < 8; ++iter) {
    bool changed = false;

    // Walk statements and refine.
    std::function<void(const std::vector<Statement> &)> walk;
    walk = [&](const std::vector<Statement> & block) {
      for (const auto & stmt : block) {
        std::visit(
          [&](const auto & elem) {
            using S = std::decay_t<decltype(elem)>;
            if constexpr (std::is_same_v<S, AssignmentStmt>) {
              // Constrain RHS against target type when known.
              if (const Type * tgt = ctx.get_type(elem.target)) {
                if (!is_unresolved_type_var(*tgt)) {
                  changed |= try_constrain_expr(elem.value, *tgt);
                } else {
                  // If target is unresolved, but RHS is concrete, copy it.
                  auto get_global = [](std::string_view) -> const Type * { return nullptr; };
                  auto rhs = infer_expression_type(elem.value, ctx, get_global);
                  if (!rhs.has_error()) {
                    Type rhs_t = rhs.type;
                    if (env_) rhs_t = env_->resolve(rhs_t);
                    if (!is_unresolved_type_var(rhs_t) && !rhs_t.is_unknown() && !rhs_t.is_any()) {
                      ctx.set_type(elem.target, apply_nullable_requirement(std::move(rhs_t), *tgt));
                      changed = true;
                    }
                  }
                }
              }

              // Symmetric refinement: if RHS is a VarRef to unresolved and target is concrete.
              if (const auto * vr = std::get_if<VarRef>(&elem.value)) {
                if (const Type * tgt = ctx.get_type(elem.target)) {
                  if (!is_unresolved_type_var(*tgt)) {
                    if (const Type * cur = ctx.get_type(vr->name)) {
                      if (is_unresolved_type_var(*cur)) {
                        ctx.set_type(vr->name, apply_nullable_requirement(*tgt, *cur));
                        changed = true;
                      }
                    }
                  }
                }
              }
            } else if constexpr (std::is_same_v<S, Box<NodeStmt>>) {
              // Reuse existing port-based inference, but allow refinement.
              for (const auto & arg : elem->args) {
                if (!arg.name) {
                  continue;
                }
                const PortInfo * port = nodes_.get_port(elem->node_name, *arg.name);
                if (!port || !port->type_name) {
                  continue;
                }
                Type port_type = Type::from_string(*port->type_name);
                if (env_) port_type = env_->resolve(port_type);

                std::visit(
                  [&](const auto & av) {
                    using AV = std::decay_t<decltype(av)>;
                    if constexpr (std::is_same_v<AV, InlineBlackboardDecl>) {
                      const Type * cur = ctx.get_type(av.name);
                      if (!cur) {
                        ctx.set_type(av.name, port_type);
                        changed = true;
                      } else if (is_unresolved_type_var(*cur)) {
                        ctx.set_type(av.name, apply_nullable_requirement(port_type, *cur));
                        changed = true;
                      }
                    } else if constexpr (std::is_same_v<AV, Expression>) {
                      if (const auto * vr = std::get_if<VarRef>(&av)) {
                        const Type * cur = ctx.get_type(vr->name);
                        if (!cur) {
                          ctx.set_type(vr->name, port_type);
                          changed = true;
                        } else if (is_unresolved_type_var(*cur)) {
                          ctx.set_type(vr->name, apply_nullable_requirement(port_type, *cur));
                          changed = true;
                        }
                      }

                      // Also constrain nested expressions against expected port type for in-ports.
                      if (port->direction == PortDirection::In) {
                        changed |= try_constrain_expr(av, port_type);
                      }
                    }
                  },
                  arg.value);
              }

              if (elem->has_children_block) {
                walk(elem->children);
              }
            } else if constexpr (std::is_same_v<S, BlackboardDeclStmt>) {
              // If declared without type/initializer, seed as type var.
              if (!elem.type_name && !elem.initial_value) {
                if (!ctx.has_type(elem.name)) {
                  ctx.set_type(elem.name, Type::type_var(/*nullable_requirement=*/false));
                  changed = true;
                }
              }
            }
          },
          stmt);
      }
    };

    walk(tree.body);

    if (!changed) {
      break;
    }
  }

  // Defaulting placeholder (see note above).
  default_literals_in_ctx();

  current_scope_ = saved_scope;
  return ctx;
}

Type TypeResolver::infer_literal_type(const Literal & lit)
{
  return std::visit(
    [](const auto & val) -> Type {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, StringLiteral>) {
        return Type::string_literal_bytes(val.value.size());
      } else if constexpr (std::is_same_v<T, IntLiteral>) {
        return Type::integer_literal(val.value);
      } else if constexpr (std::is_same_v<T, FloatLiteral>) {
        return Type::float_literal(val.value);
      } else if constexpr (std::is_same_v<T, BoolLiteral>) {
        return Type::bool_type();
      } else if constexpr (std::is_same_v<T, NullLiteral>) {
        return Type::null_literal();
      } else {
        return Type::unknown();
      }
    },
    lit);
}

TypeInferenceResult TypeResolver::infer_expression_type(
  const Expression & expr, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type) const
{
  return std::visit(
    [&](const auto & val) -> TypeInferenceResult {
      using T = std::decay_t<decltype(val)>;

      if constexpr (std::is_same_v<T, Literal>) {
        return TypeInferenceResult::success(infer_literal_type(val));
      } else if constexpr (std::is_same_v<T, VarRef>) {
        // Look up variable type
        if (const Type * t = ctx.get_type(val.name)) {
          return TypeInferenceResult::success(*t);
        }
        if (get_global_type) {
          if (const Type * t = get_global_type(val.name)) {
            return TypeInferenceResult::success(*t);
          }
        }
        return TypeInferenceResult::failure(Type::unknown(), "Unknown variable: " + val.name);
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const BinaryExpr & binary = *val;
        auto left_result = infer_expression_type(binary.left, ctx, get_global_type);
        auto right_result = infer_expression_type(binary.right, ctx, get_global_type);

        if (left_result.has_error()) return left_result;
        if (right_result.has_error()) return right_result;

        // Determine result type based on operator
        switch (binary.op) {
          // Comparison operators return bool
          case BinaryOp::Eq:
          case BinaryOp::Ne:
          case BinaryOp::Lt:
          case BinaryOp::Le:
          case BinaryOp::Gt:
          case BinaryOp::Ge: {
            // Be permissive around error-recovery types.
            if (
              left_result.type.is_any() || right_result.type.is_any() ||
              left_result.type.is_unknown() || right_result.type.is_unknown()) {
              return TypeInferenceResult::success(Type::bool_type());
            }

            // Equality/inequality: require comparable types.
            if (binary.op == BinaryOp::Eq || binary.op == BinaryOp::Ne) {
              const bool lhs_null = std::holds_alternative<TypeNullLiteral>(left_result.type.value);
              const bool rhs_null =
                std::holds_alternative<TypeNullLiteral>(right_result.type.value);
              if (lhs_null) {
                if (!right_result.type.is_nullable()) {
                  return TypeInferenceResult::failure(
                    Type::bool_type(), "Equality comparison with null requires a nullable operand");
                }
                return TypeInferenceResult::success(Type::bool_type());
              }
              if (rhs_null) {
                if (!left_result.type.is_nullable()) {
                  return TypeInferenceResult::failure(
                    Type::bool_type(), "Equality comparison with null requires a nullable operand");
                }
                return TypeInferenceResult::success(Type::bool_type());
              }

              const bool ok = left_result.type.is_compatible_with(right_result.type) ||
                              right_result.type.is_compatible_with(left_result.type);
              if (!ok) {
                return TypeInferenceResult::failure(
                  Type::bool_type(), "Equality comparison requires compatible operand types");
              }
              return TypeInferenceResult::success(Type::bool_type());
            }

            // Ordering comparisons: require non-nullable numeric operands with a
            // common type reachable via widening.
            if (
              !left_result.type.is_numeric() || !right_result.type.is_numeric() ||
              left_result.type.is_nullable() || right_result.type.is_nullable()) {
              return TypeInferenceResult::failure(
                Type::bool_type(), "Ordering comparison requires non-nullable numeric operands");
            }

            auto common = common_numeric_type(left_result.type, right_result.type);
            if (common.error.has_value()) {
              return TypeInferenceResult::failure(Type::bool_type(), *common.error);
            }
            return TypeInferenceResult::success(Type::bool_type());
          }

          // Logical operators require and return bool
          case BinaryOp::And:
          case BinaryOp::Or:
            if (
              !std::holds_alternative<TypeBool>(left_result.type.value) ||
              !std::holds_alternative<TypeBool>(right_result.type.value)) {
              return TypeInferenceResult::failure(
                Type::bool_type(), "Logical operators require bool operands");
            }
            return TypeInferenceResult::success(Type::bool_type());

          // Bitwise operators require integer operands
          case BinaryOp::BitAnd:
          case BinaryOp::BitOr: {
            if (!left_result.type.is_integer() || !right_result.type.is_integer()) {
              return TypeInferenceResult::failure(
                Type::unknown(), "Bitwise operators require integer operands");
            }

            // constant-fold literal & literal / literal | literal
            if (const auto * li = std::get_if<TypeIntegerLiteral>(&left_result.type.value)) {
              if (const auto * ri = std::get_if<TypeIntegerLiteral>(&right_result.type.value)) {
                const int64_t out = (binary.op == BinaryOp::BitAnd) ? (li->value & ri->value)
                                                                    : (li->value | ri->value);
                return TypeInferenceResult::success(Type::integer_literal(out));
              }
            }

            auto common = common_numeric_type(left_result.type, right_result.type);
            if (common.error.has_value()) {
              return TypeInferenceResult::failure(Type::unknown(), *common.error);
            }
            return TypeInferenceResult::success(std::move(common.type));
          }

          // Arithmetic operators
          case BinaryOp::Add:
          case BinaryOp::Sub:
          case BinaryOp::Mul:
          case BinaryOp::Div:
          case BinaryOp::Mod: {
            // Special case: string + string
            if (binary.op == BinaryOp::Add) {
              const bool lhs_str =
                std::holds_alternative<TypeString>(left_result.type.value) ||
                std::holds_alternative<TypeStringLiteral>(left_result.type.value);
              const bool rhs_str =
                std::holds_alternative<TypeString>(right_result.type.value) ||
                std::holds_alternative<TypeStringLiteral>(right_result.type.value);
              if (lhs_str && rhs_str) {
                return TypeInferenceResult::success(Type::string_type());
              }
            }

            if (!left_result.type.is_numeric() || !right_result.type.is_numeric()) {
              return TypeInferenceResult::failure(
                Type::unknown(), "Operator cannot be applied to non-numeric types");
            }

            // Mod requires integer operands (per reference: expression-typing.md §3.4.2)
            if (binary.op == BinaryOp::Mod) {
              if (!left_result.type.is_integer() || !right_result.type.is_integer()) {
                return TypeInferenceResult::failure(
                  Type::unknown(), "Modulo operator requires integer operands");
              }
            }

            // constant-fold literal+literal (useful for const_expr)
            if (const auto * li = std::get_if<TypeIntegerLiteral>(&left_result.type.value)) {
              if (const auto * ri = std::get_if<TypeIntegerLiteral>(&right_result.type.value)) {
                int64_t out = 0;
                const auto overflow_or_domain = [&]() {
                  return TypeInferenceResult::failure(
                    Type::integer_literal(0),
                    "Integer overflow or invalid operation in constant expression");
                };

                switch (binary.op) {
                  case BinaryOp::Add:
                    if (!checked_add_i64(li->value, ri->value, out)) return overflow_or_domain();
                    break;
                  case BinaryOp::Sub:
                    if (!checked_sub_i64(li->value, ri->value, out)) return overflow_or_domain();
                    break;
                  case BinaryOp::Mul:
                    if (!checked_mul_i64(li->value, ri->value, out)) return overflow_or_domain();
                    break;
                  case BinaryOp::Div:
                    if (!checked_div_i64(li->value, ri->value, out)) {
                      return TypeInferenceResult::failure(
                        Type::integer_literal(0),
                        "Division by zero or overflow in constant expression");
                    }
                    break;
                  case BinaryOp::Mod:
                    if (!checked_mod_i64(li->value, ri->value, out)) {
                      return TypeInferenceResult::failure(
                        Type::integer_literal(0),
                        "Modulo by zero or overflow in constant expression");
                    }
                    break;
                  default:
                    break;
                }
                return TypeInferenceResult::success(Type::integer_literal(out));
              }
            }

            if (const auto * lf = std::get_if<TypeFloatLiteral>(&left_result.type.value)) {
              if (const auto * rf = std::get_if<TypeFloatLiteral>(&right_result.type.value)) {
                double out = 0.0;
                switch (binary.op) {
                  case BinaryOp::Add:
                    out = lf->value + rf->value;
                    break;
                  case BinaryOp::Sub:
                    out = lf->value - rf->value;
                    break;
                  case BinaryOp::Mul:
                    out = lf->value * rf->value;
                    break;
                  case BinaryOp::Div:
                    out = lf->value / rf->value;
                    break;
                  // Note: BinaryOp::Mod is not allowed for float types (per reference)
                  default:
                    break;
                }

                if (!std::isfinite(out)) {
                  return TypeInferenceResult::failure(
                    Type::float_literal(0.0),
                    "Float overflow or invalid operation in constant expression");
                }

                return TypeInferenceResult::success(Type::float_literal(out));
              }
            }

            // promote numeric result
            auto common = common_numeric_type(left_result.type, right_result.type);
            if (common.error.has_value()) {
              return TypeInferenceResult::failure(Type::unknown(), *common.error);
            }
            return TypeInferenceResult::success(std::move(common.type));
          }

          default:
            return TypeInferenceResult::success(Type::unknown());
        }
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const UnaryExpr & unary = *val;
        auto operand_result = infer_expression_type(unary.operand, ctx, get_global_type);

        if (operand_result.has_error()) return operand_result;

        switch (unary.op) {
          case UnaryOp::Not:
            if (!std::holds_alternative<TypeBool>(operand_result.type.value)) {
              return TypeInferenceResult::failure(
                Type::bool_type(), "Logical not requires bool operand");
            }
            return TypeInferenceResult::success(Type::bool_type());

          case UnaryOp::Neg:
            if (!operand_result.type.is_numeric()) {
              return TypeInferenceResult::failure(
                Type::unknown(), "Negation requires numeric operand");
            }
            if (const auto * li = std::get_if<TypeIntegerLiteral>(&operand_result.type.value)) {
              return TypeInferenceResult::success(Type::integer_literal(-li->value));
            }
            if (const auto * lf = std::get_if<TypeFloatLiteral>(&operand_result.type.value)) {
              return TypeInferenceResult::success(Type::float_literal(-lf->value));
            }
            return TypeInferenceResult::success(operand_result.type);

          default:
            return TypeInferenceResult::success(operand_result.type);
        }
      } else {
        // Cast
        if constexpr (std::is_same_v<T, Box<CastExpr>>) {
          const CastExpr & cast = *val;
          auto src_res = infer_expression_type(cast.expr, ctx, get_global_type);
          if (src_res.has_error()) return src_res;

          TypeParseResult parsed = Type::parse(cast.type_name);
          if (parsed.has_error()) {
            return TypeInferenceResult::failure(Type::unknown(), *parsed.error);
          }

          Type target = std::move(parsed.type);
          if (std::holds_alternative<TypeInfer>(target.value)) {
            return TypeInferenceResult::failure(
              Type::unknown(), "Cannot cast to inferred type '_' (use a concrete type)");
          }
          if (env_) {
            target = env_->resolve(target);
          }

          Type result_type = target;

          // Handle vec<_> / [_; N] style infer within casts
          if (const auto * vec_t = std::get_if<TypeVec>(&target.value)) {
            if (std::holds_alternative<TypeInfer>(vec_t->element->value)) {
              // vec<_>: infer element from source
              if (const auto * src_arr = std::get_if<TypeStaticArray>(&src_res.type.value)) {
                result_type = Type::vec(*src_arr->element);
              }
              if (const auto * src_vec = std::get_if<TypeVec>(&src_res.type.value)) {
                result_type = Type::vec(*src_vec->element);
              }
            }
          }

          // Validate cast legality (explicit casts still obey extern-type rules, etc.).
          Type src_t = src_res.type;
          Type dst_t = result_type;
          if (env_) {
            src_t = env_->resolve(src_t);
            dst_t = env_->resolve(dst_t);
          }

          if (!is_cast_allowed_impl(src_t, dst_t)) {
            return TypeInferenceResult::failure(
              Type::unknown(),
              "Invalid cast: cannot cast " + src_t.to_string() + " to " + dst_t.to_string());
          }

          return TypeInferenceResult::success(std::move(result_type));
        }

        // Index
        if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
          const IndexExpr & idx = *val;
          auto base_res = infer_expression_type(idx.base, ctx, get_global_type);
          if (base_res.has_error()) return base_res;

          auto index_res = infer_expression_type(idx.index, ctx, get_global_type);
          if (index_res.has_error()) return index_res;

          // Spec: index must be integer.
          if (
            !index_res.type.is_any() && !index_res.type.is_unknown() &&
            !index_res.type.is_integer()) {
            return TypeInferenceResult::failure(
              Type::unknown(), "Index expression requires an integer index");
          }

          if (const auto * arr = std::get_if<TypeStaticArray>(&base_res.type.value)) {
            // Spec (expression-typing.md §3.4.4): bounds check for constant index on static array.
            std::optional<int64_t> array_size;
            if (const auto * n = std::get_if<uint64_t>(&arr->size.value)) {
              if (*n <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                array_size = static_cast<int64_t>(*n);
              }
            } else if (const auto * ident = std::get_if<std::string>(&arr->size.value)) {
              if (current_scope_) {
                SimpleConstEval ev{symbols_, current_scope_, env_, /*memo=*/{}, /*in_stack=*/{}};
                VarRef vr;
                vr.name = *ident;
                vr.direction = std::nullopt;
                vr.range = SourceRange{};
                const Expression size_expr = vr;
                if (auto size_val = ev.eval(size_expr);
                    size_val.has_value() && size_val.value() >= 0) {
                  array_size = size_val;
                }
              }
            }

            std::optional<int64_t> index_val;
            if (current_scope_) {
              SimpleConstEval ev{symbols_, current_scope_, env_, /*memo=*/{}, /*in_stack=*/{}};
              index_val = ev.eval(idx.index);
            }
            if (!index_val) {
              if (const auto * idx_lit = std::get_if<TypeIntegerLiteral>(&index_res.type.value)) {
                index_val = idx_lit->value;
              }
            }

            if (array_size && index_val) {
              const int64_t n = *array_size;
              const int64_t i = *index_val;
              if (i < 0 || i >= n) {
                return TypeInferenceResult::failure(
                  *arr->element, "Array index " + std::to_string(i) +
                                   " is out of bounds for array of size " + std::to_string(n));
              }
            }
            return TypeInferenceResult::success(*arr->element);
          }
          if (const auto * vec = std::get_if<TypeVec>(&base_res.type.value)) {
            return TypeInferenceResult::success(*vec->element);
          }
          return TypeInferenceResult::failure(Type::unknown(), "Indexing requires an array type");
        }

        // Array literal
        if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
          const ArrayLiteralExpr & arr = *val;
          // repeat-init
          if (arr.repeat_value && arr.repeat_count) {
            auto val_res = infer_expression_type(*arr.repeat_value, ctx, get_global_type);
            if (val_res.has_error()) return val_res;

            TypeArraySizeExpr sz;

            // Reference: repeat-init requires const_expr for static arrays.
            // We attempt const evaluation here to infer the exact array length.
            // If evaluation fails, keep a best-effort unknown size (diagnostics are
            // emitted by the semantic validator).
            if (current_scope_) {
              SimpleConstEval ev{symbols_, current_scope_, env_, /*memo=*/{}, /*in_stack=*/{}};
              if (auto n = ev.eval(*arr.repeat_count)) {
                if (*n >= 0) {
                  sz.value = static_cast<uint64_t>(*n);
                } else {
                  sz.value = static_cast<uint64_t>(0);
                }
              } else {
                sz.value = static_cast<uint64_t>(0);
              }
            } else {
              // Fallback: only literal sizes.
              auto count_res = infer_expression_type(*arr.repeat_count, ctx, get_global_type);
              uint64_t n = 0;
              if (!count_res.has_error()) {
                if (const auto * ci = std::get_if<TypeIntegerLiteral>(&count_res.type.value)) {
                  if (ci->value >= 0) {
                    n = static_cast<uint64_t>(ci->value);
                  }
                }
              }
              sz.value = n;
            }
            return TypeInferenceResult::success(
              Type::static_array(std::move(val_res.type), TypeArraySizeKind::Exact, std::move(sz)));
          }

          // element list
          if (arr.elements.empty()) {
            TypeArraySizeExpr sz;
            sz.value = static_cast<uint64_t>(0);
            return TypeInferenceResult::success(
              Type::static_array(Type::infer(false), TypeArraySizeKind::Exact, std::move(sz)));
          }

          Type elem_ty = Type::unknown();
          for (const auto & e : arr.elements) {
            auto r = infer_expression_type(e, ctx, get_global_type);
            if (r.has_error()) return r;
            if (elem_ty.is_unknown()) {
              elem_ty = r.type;
              continue;
            }
            // crude unification: if either is float, use float64; else int32; else keep first.
            if (elem_ty.is_numeric() && r.type.is_numeric()) {
              if (elem_ty.is_float() || r.type.is_float()) {
                elem_ty = Type::float_type(64);
              } else {
                elem_ty = Type::int_type(true, 32);
              }
            }
          }

          TypeArraySizeExpr sz;
          sz.value = static_cast<uint64_t>(arr.elements.size());
          return TypeInferenceResult::success(
            Type::static_array(std::move(elem_ty), TypeArraySizeKind::Exact, std::move(sz)));
        }

        // vec! macro
        if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
          const VecMacroExpr & vm = *val;
          // reuse array literal inference
          const Expression arr_expr = Box<ArrayLiteralExpr>(vm.value);
          auto arr_res = infer_expression_type(arr_expr, ctx, get_global_type);
          if (arr_res.has_error()) return arr_res;
          if (const auto * arr_t = std::get_if<TypeStaticArray>(&arr_res.type.value)) {
            return TypeInferenceResult::success(Type::vec(*arr_t->element));
          }
          return TypeInferenceResult::success(Type::vec(Type::unknown()));
        }

        return TypeInferenceResult::success(Type::unknown());
      }
    },
    expr);
}

void TypeResolver::infer_from_node_usage(const NodeStmt & node, TypeContext & ctx)
{
  // Process arguments for type inference
  for (const auto & arg : node.args) {
    process_argument_for_inference(arg, node.node_name, ctx);
  }

  // Recurse into children statement blocks.
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          infer_from_node_usage(*elem, ctx);
        } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
          if (elem.type_name) {
            Type t = Type::from_string(*elem.type_name);
            if (env_) t = env_->resolve(t);
            ctx.set_type(elem.name, std::move(t));
          } else if (elem.initial_value) {
            auto get_global = [](std::string_view) -> const Type * { return nullptr; };
            auto result = infer_expression_type(*elem.initial_value, ctx, get_global);
            Type t = finalize_default_type(std::move(result.type));
            if (env_) t = env_->resolve(t);
            ctx.set_type(elem.name, std::move(t));
          } else {
            // `var x;` inside children blocks starts as an unresolved internal type variable.
            // Constraints from later assignments/uses should resolve it.
            ctx.set_type(elem.name, Type::type_var(false));
          }
        } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
          if (elem.type_name) {
            Type t = Type::from_string(*elem.type_name);
            if (env_) t = env_->resolve(t);
            ctx.set_type(elem.name, std::move(t));
          } else {
            auto get_global = [](std::string_view) -> const Type * { return nullptr; };
            auto result = infer_expression_type(elem.value, ctx, get_global);
            Type t = std::move(result.type);
            if (env_) t = env_->resolve(t);
            ctx.set_type(elem.name, std::move(t));
          }
        }
      },
      child);
  }
}

void TypeResolver::process_argument_for_inference(
  const Argument & arg, std::string_view node_name, TypeContext & ctx)
{
  // Skip if port name is not specified (Argument.name is the port name)
  if (!arg.name) return;

  // Get port info from registry
  const PortInfo * port = nodes_.get_port(node_name, *arg.name);
  if (!port || !port->type_name) return;

  Type port_type = Type::from_string(*port->type_name);
  if (env_) port_type = env_->resolve(port_type);

  // Infer variable types from argument expressions and inline out-var decls.
  std::visit(
    [&](const auto & val) {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, InlineBlackboardDecl>) {
        if (!ctx.has_type(val.name)) {
          ctx.set_type(val.name, port_type);
        }
      } else if constexpr (std::is_same_v<T, Expression>) {
        if (const auto * ref = std::get_if<VarRef>(&val)) {
          if (!ctx.has_type(ref->name)) {
            ctx.set_type(ref->name, port_type);
          }
        }
      }
    },
    arg.value);
}

// ============================================================================
// TypeChecker Implementation
// ============================================================================

namespace
{

Type resolve_if_needed(const TypeEnvironment * env, Type t)
{
  if (!env) return t;
  return env->resolve(t);
}

bool type_contains_infer(const Type & t)
{
  if (std::holds_alternative<TypeInfer>(t.value)) {
    return true;
  }
  if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
    return type_contains_infer(*n->base);
  }
  if (const auto * v = std::get_if<TypeVec>(&t.value)) {
    return type_contains_infer(*v->element);
  }
  if (const auto * a = std::get_if<TypeStaticArray>(&t.value)) {
    return type_contains_infer(*a->element);
  }
  return false;
}

TypeInferenceResult infer_expr_type(
  const Expression & expr, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type,
  const SymbolTable * symbols, const NodeRegistry * nodes, const Scope * scope,
  const TypeEnvironment * env)
{
  // Reuse TypeResolver logic without relying on node usage inference.
  if (symbols && nodes) {
    TypeResolver resolver(*symbols, *nodes, env);
    resolver.set_scope_for_const_eval(scope);
    return resolver.infer_expression_type(expr, ctx, get_global_type);
  }

  const SymbolTable empty_symbols;
  const NodeRegistry empty_nodes;
  TypeResolver resolver(empty_symbols, empty_nodes, env);
  return resolver.infer_expression_type(expr, ctx, get_global_type);
}

}  // namespace

void TypeChecker::check_tree(
  const TreeDef & tree, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type,
  DiagnosticBag & diagnostics)
{
  // Parameters have explicit type syntax; any remaining inference wildcard
  // here is an error.
  for (const auto & param : tree.params) {
    if (const Type * t = ctx.get_type(param.name)) {
      const Type resolved = resolve_if_needed(env_, *t);
      if (type_contains_infer(resolved)) {
        diagnostics.error(
          param.range,
          "Unresolved inferred type for parameter '" + param.name + "' (use a concrete type)");
      }
    }
  }

  std::function<bool(const Expression &, const Type &, std::string_view)> check_expr_against;
  check_expr_against =
    [&](const Expression & expr, const Type & target, std::string_view context_msg) -> bool {
    Type tgt = resolve_if_needed(env_, target);

    // Special-case array literals: must be checked bidirectionally.
    if (const auto * arr_lit = std::get_if<Box<ArrayLiteralExpr>>(&expr)) {
      if (const auto * tgt_arr = std::get_if<TypeStaticArray>(&tgt.value)) {
        const auto & lit = **arr_lit;
        // Determine literal length when possible
        std::optional<uint64_t> lit_len;
        if (!lit.elements.empty()) {
          lit_len = static_cast<uint64_t>(lit.elements.size());
        } else if (lit.repeat_count) {
          auto count_t = infer_expr_type(
            *lit.repeat_count, ctx, get_global_type, symbols_, nodes_, scope_, env_);
          if (!count_t.has_error()) {
            if (const auto * ci = std::get_if<TypeIntegerLiteral>(&count_t.type.value)) {
              if (ci->value >= 0) lit_len = static_cast<uint64_t>(ci->value);
            }
          }
        }

        const auto * bound = std::get_if<uint64_t>(&tgt_arr->size.value);
        if (tgt_arr->size_kind == TypeArraySizeKind::Exact) {
          if (bound && lit_len && *lit_len != *bound) {
            diagnostics.error(
              lit.range, std::string(context_msg) + ": array length mismatch (expected " +
                           std::to_string(*bound) + ", got " + std::to_string(*lit_len) + ")");
            return false;
          }
        } else {
          if (bound && lit_len && *lit_len > *bound) {
            diagnostics.error(
              lit.range, std::string(context_msg) + ": array length exceeds bound (<= " +
                           std::to_string(*bound) + ", got " + std::to_string(*lit_len) + ")");
            return false;
          }
        }

        // Element checks
        if (!lit.elements.empty()) {
          for (const auto & e : lit.elements) {
            if (!check_expr_against(e, *tgt_arr->element, "array element")) {
              return false;
            }
          }
        } else if (lit.repeat_value) {
          if (!check_expr_against(*lit.repeat_value, *tgt_arr->element, "array repeat value")) {
            return false;
          }
        }
        return true;
      }

      // static array literal cannot implicitly become vec<T>
      if (std::holds_alternative<TypeVec>(tgt.value)) {
        diagnostics.error(
          (**arr_lit).range, std::string(context_msg) +
                               ": cannot implicitly convert static array literal to vec<T> (use "
                               "vec![...] or 'as vec<_>')");
        return false;
      }
    }

    // vec! macro
    if (const auto * vec_lit = std::get_if<Box<VecMacroExpr>>(&expr)) {
      if (const auto * tgt_vec = std::get_if<TypeVec>(&tgt.value)) {
        const auto & vm = **vec_lit;
        // Check array literal payload against element type.
        if (!vm.value.elements.empty()) {
          for (const auto & e : vm.value.elements) {
            if (!check_expr_against(e, *tgt_vec->element, "vec element")) {
              return false;
            }
          }
        } else if (vm.value.repeat_value) {
          if (!check_expr_against(*vm.value.repeat_value, *tgt_vec->element, "vec repeat value")) {
            return false;
          }
        }

        if (vm.value.repeat_count) {
          auto count_t = infer_expr_type(
            *vm.value.repeat_count, ctx, get_global_type, symbols_, nodes_, scope_, env_);
          if (count_t.has_error()) {
            diagnostics.error(
              get_range(*vm.value.repeat_count),
              std::string(context_msg) + ": invalid vec repeat count expression");
            return false;
          }
          Type ct = resolve_if_needed(env_, std::move(count_t.type));
          if (!ct.is_any() && !ct.is_unknown() && !ct.is_integer()) {
            diagnostics.error(
              get_range(*vm.value.repeat_count),
              std::string(context_msg) + ": vec repeat count must be an integer");
            return false;
          }
          if (const auto * ci = std::get_if<TypeIntegerLiteral>(&ct.value)) {
            if (ci->value < 0) {
              diagnostics.error(
                get_range(*vm.value.repeat_count),
                std::string(context_msg) + ": vec repeat count must be non-negative");
              return false;
            }
          }
        }
        return true;
      }
      // vec literal must match vec target
      diagnostics.error(
        (**vec_lit).range,
        std::string(context_msg) + ": vec![...] expression requires vec<T> target");
      return false;
    }

    // General case: infer type and check assignability.
    auto src_res = infer_expr_type(expr, ctx, get_global_type, symbols_, nodes_, scope_, env_);
    if (src_res.has_error()) {
      diagnostics.error(get_range(expr), *src_res.error);
      return false;
    }
    const Type src = resolve_if_needed(env_, std::move(src_res.type));

    // Improve literal assignability precision
    // (infer_expr_type already returns internal literal types where possible)
    if (!tgt.is_compatible_with(src)) {
      diagnostics.error(
        get_range(expr), "Type mismatch: " + std::string(context_msg) + ": cannot assign " +
                           src.to_string() + " to " + tgt.to_string());
      return false;
    }
    return true;
  };

  const auto check_block = [&](const std::vector<Statement> & block, auto & self_ref) -> void {
    for (const auto & stmt : block) {
      std::visit(
        [&](const auto & elem) {
          using T = std::decay_t<decltype(elem)>;
          if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
            check_node_stmt(*elem, ctx, get_global_type, diagnostics);
          } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
            if (elem.type_name && elem.initial_value) {
              // Prefer the resolved type from the inference/type-resolution pass
              // (includes alias expansion and bounded-size normalization).
              Type declared = [&]() -> Type {
                if (const Type * resolved_decl = ctx.get_type(elem.name)) {
                  return resolve_if_needed(env_, *resolved_decl);
                }
                return resolve_if_needed(env_, Type::from_string(*elem.type_name));
              }();

              // If the annotation is '_' / '_?', prefer the resolved type from
              // the inference context (when available).
              if (std::holds_alternative<TypeInfer>(declared.value)) {
                if (const Type * resolved_decl = ctx.get_type(elem.name)) {
                  const Type resolved = resolve_if_needed(env_, *resolved_decl);
                  if (!type_contains_infer(resolved)) {
                    declared = resolved;
                  }
                }
              }

              if (!type_contains_infer(declared)) {
                (void)check_expr_against(*elem.initial_value, declared, "initializer");
              }
            } else if (!elem.type_name && !elem.initial_value) {
              diagnostics.error(
                elem.range,
                "Local variable '" + elem.name + "' must have either a type or initial value");
            } else if (!elem.type_name && elem.initial_value) {
              // Reference: docs/reference/type-system/inference-and-resolution.md
              // Unresolved inference variables (including null-derived _?) must not
              // remain at scope end.
              if (const Type * inferred = ctx.get_type(elem.name)) {
                const Type resolved = resolve_if_needed(env_, *inferred);
                if (type_contains_infer(resolved)) {
                  diagnostics.error(
                    elem.range, "Unresolved inferred type for '" + elem.name +
                                  "' (add a type annotation or constrain its usage)");
                }
              }
            }

            // Any wildcard in the annotation must be resolved by scope end.
            if (elem.type_name) {
              const Type ann = resolve_if_needed(env_, Type::from_string(*elem.type_name));
              if (type_contains_infer(ann)) {
                const Type * inferred = ctx.get_type(elem.name);
                const Type resolved = inferred ? resolve_if_needed(env_, *inferred) : ann;
                if (type_contains_infer(resolved)) {
                  diagnostics.error(
                    elem.range, "Unresolved inferred type for '" + elem.name +
                                  "' (use a concrete type or provide a constraining initializer)");
                }
              }
            }
          } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
            if (elem.type_name) {
              Type declared = [&]() -> Type {
                if (const Type * resolved_decl = ctx.get_type(elem.name)) {
                  return resolve_if_needed(env_, *resolved_decl);
                }
                return resolve_if_needed(env_, Type::from_string(*elem.type_name));
              }();
              if (std::holds_alternative<TypeInfer>(declared.value)) {
                if (const Type * resolved_decl = ctx.get_type(elem.name)) {
                  const Type resolved = resolve_if_needed(env_, *resolved_decl);
                  if (!type_contains_infer(resolved)) {
                    declared = resolved;
                  }
                }
              }

              if (!type_contains_infer(declared)) {
                (void)check_expr_against(elem.value, declared, "const initializer");
              }
            } else {
              // const without type annotation: infer from value, but it must be
              // fully resolved by scope end.
              if (const Type * inferred = ctx.get_type(elem.name)) {
                const Type resolved = resolve_if_needed(env_, *inferred);
                if (type_contains_infer(resolved)) {
                  diagnostics.error(
                    elem.range, "Unresolved inferred type for const '" + elem.name +
                                  "' (add a type annotation or constrain its usage)");
                }
              }
            }

            if (elem.type_name) {
              const Type ann = resolve_if_needed(env_, Type::from_string(*elem.type_name));
              if (type_contains_infer(ann)) {
                const Type * inferred = ctx.get_type(elem.name);
                const Type resolved = inferred ? resolve_if_needed(env_, *inferred) : ann;
                if (type_contains_infer(resolved)) {
                  diagnostics.error(
                    elem.range, "Unresolved inferred type for const '" + elem.name +
                                  "' (use a concrete type or provide a constraining initializer)");
                }
              }
            }
          } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
            const Type * target_type = ctx.get_type(elem.target);
            if (!target_type && get_global_type) {
              target_type = get_global_type(elem.target);
            }
            if (!target_type) {
              return;
            }
            (void)check_expr_against(elem.value, *target_type, "assignment");
          }
        },
        stmt);
    }
    (void)self_ref;
  };

  check_block(tree.body, check_block);
}

void TypeChecker::check_node_stmt(
  const NodeStmt & node, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type,
  DiagnosticBag & diagnostics)
{
  // Flow-sensitive narrowing for @guard/@run_while scopes.
  // Reference: docs/reference/static-analysis-and-safety.md 6.2.
  auto collect_non_null_vars = [](
                                 const Expression & expr, bool negate, auto & self,
                                 std::unordered_set<std::string> & out) -> void {
    if (const auto * u = std::get_if<Box<UnaryExpr>>(&expr)) {
      if ((**u).op == UnaryOp::Not) {
        self((**u).operand, !negate, self, out);
      }
      return;
    }
    if (const auto * b = std::get_if<Box<BinaryExpr>>(&expr)) {
      const auto & be = **b;
      if (!negate && be.op == BinaryOp::And) {
        self(be.left, false, self, out);
        self(be.right, false, self, out);
        return;
      }

      BinaryOp op = be.op;
      if (negate) {
        if (op == BinaryOp::Eq) {
          op = BinaryOp::Ne;
        } else if (op == BinaryOp::Ne) {
          op = BinaryOp::Eq;
        }
      }

      if (op == BinaryOp::Ne) {
        const auto is_null = [](const Expression & e) -> bool {
          if (const auto * lit = std::get_if<Literal>(&e)) {
            return std::holds_alternative<NullLiteral>(*lit);
          }
          return false;
        };

        if (const auto * vr = std::get_if<VarRef>(&be.left); vr && is_null(be.right)) {
          out.insert(vr->name);
          return;
        }
        if (const auto * vr = std::get_if<VarRef>(&be.right); vr && is_null(be.left)) {
          out.insert(vr->name);
          return;
        }
      }
      return;
    }
  };

  auto narrowed_type = [](const Type & t) -> std::optional<Type> {
    if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
      return *n->base;
    }
    return std::nullopt;
  };

  std::unordered_set<std::string> non_null;
  non_null.reserve(8);
  for (const auto & pc : node.preconditions) {
    if (pc.kind == "guard" || pc.kind == "run_while") {
      collect_non_null_vars(pc.condition, false, collect_non_null_vars, non_null);
    }
  }

  // Prepare narrowed contexts for recursive checking.
  TypeContext narrowed_ctx = ctx;
  std::unordered_map<std::string, Type> global_overrides;
  if (!non_null.empty()) {
    for (const auto & name : non_null) {
      if (const Type * t = ctx.get_type(name)) {
        if (auto nt = narrowed_type(*t)) {
          narrowed_ctx.set_type(name, std::move(*nt));
        }
      }

      if (get_global_type) {
        if (const Type * gt = get_global_type(name)) {
          if (auto nt = narrowed_type(*gt)) {
            global_overrides.emplace(std::string(name), std::move(*nt));
          }
        }
      }
    }
  }

  const auto narrowed_get_global = [&](std::string_view name) -> const Type * {
    if (auto it = global_overrides.find(std::string(name)); it != global_overrides.end()) {
      return &it->second;
    }
    if (!get_global_type) {
      return nullptr;
    }
    return get_global_type(name);
  };

  // Inline out-var decls: they are declarations too, so their type must be
  // fully resolved by scope end.
  for (const auto & arg : node.args) {
    std::visit(
      [&](const auto & val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, InlineBlackboardDecl>) {
          if (const Type * inferred = ctx.get_type(val.name)) {
            const Type resolved = resolve_if_needed(env_, *inferred);
            if (type_contains_infer(resolved)) {
              diagnostics.error(
                val.range, "Unresolved inferred type for '" + val.name +
                             "' (add a type annotation or constrain its usage)");
            }
          }
        }
      },
      arg.value);
  }

  // Recurse into children statements.
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          if (non_null.empty()) {
            check_node_stmt(*elem, ctx, get_global_type, diagnostics);
          } else {
            check_node_stmt(*elem, narrowed_ctx, narrowed_get_global, diagnostics);
          }
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          // Type-check assignments inside children blocks as well.
          // This is required for reference-mandated checks like static array
          // bounds checking on const indices (expression-typing.md §3.4.4).
          const TypeContext & use_ctx = non_null.empty() ? ctx : narrowed_ctx;
          const auto & use_get_global = non_null.empty() ? get_global_type : narrowed_get_global;

          const Type * target_type = use_ctx.get_type(elem.target);
          if (!target_type && use_get_global) {
            target_type = use_get_global(elem.target);
          }
          if (!target_type) {
            return;
          }

          // General case: infer type and check assignability.
          Type tgt = resolve_if_needed(env_, *target_type);
          auto src_res =
            infer_expr_type(elem.value, use_ctx, use_get_global, symbols_, nodes_, scope_, env_);
          if (src_res.has_error()) {
            diagnostics.error(get_range(elem.value), *src_res.error);
            return;
          }
          const Type src = resolve_if_needed(env_, std::move(src_res.type));
          if (!tgt.is_compatible_with(src)) {
            diagnostics.error(
              get_range(elem.value), "Type mismatch: assignment: cannot assign " + src.to_string() +
                                       " to " + tgt.to_string());
            return;
          }
        } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
          if (!elem.type_name && !elem.initial_value) {
            diagnostics.error(
              elem.range,
              "Local variable '" + elem.name + "' must have either a type or initial value");
          } else if (!elem.type_name && elem.initial_value) {
            if (
              const Type * inferred =
                (non_null.empty() ? ctx.get_type(elem.name) : narrowed_ctx.get_type(elem.name))) {
              const Type resolved = resolve_if_needed(env_, *inferred);
              if (type_contains_infer(resolved)) {
                diagnostics.error(
                  elem.range, "Unresolved inferred type for '" + elem.name +
                                "' (add a type annotation or constrain its usage)");
              }
            }
          }

          if (elem.type_name) {
            const Type ann = resolve_if_needed(env_, Type::from_string(*elem.type_name));
            if (type_contains_infer(ann)) {
              const Type * inferred = ctx.get_type(elem.name);
              const Type resolved = inferred ? resolve_if_needed(env_, *inferred) : ann;
              if (type_contains_infer(resolved)) {
                diagnostics.error(
                  elem.range, "Unresolved inferred type for '" + elem.name +
                                "' (use a concrete type or provide a constraining initializer)");
              }
            }
          }
        } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
          if (
            const Type * inferred =
              (non_null.empty() ? ctx.get_type(elem.name) : narrowed_ctx.get_type(elem.name))) {
            const Type resolved = resolve_if_needed(env_, *inferred);
            if (type_contains_infer(resolved)) {
              diagnostics.error(
                elem.range, "Unresolved inferred type for const '" + elem.name +
                              "' (add a type annotation or constrain its usage)");
            }
          }

          if (elem.type_name) {
            const Type ann = resolve_if_needed(env_, Type::from_string(*elem.type_name));
            if (type_contains_infer(ann)) {
              const Type * inferred = ctx.get_type(elem.name);
              const Type resolved = inferred ? resolve_if_needed(env_, *inferred) : ann;
              if (type_contains_infer(resolved)) {
                diagnostics.error(
                  elem.range, "Unresolved inferred type for const '" + elem.name +
                                "' (use a concrete type or provide a constraining initializer)");
              }
            }
          }
        }
      },
      child);
  }
}

TypeInferenceResult TypeChecker::check_binary_expr(
  const BinaryExpr & expr, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type)
{
  // Keep API; best-effort inference
  return infer_expr_type(
    Expression{Box<BinaryExpr>(expr)}, ctx, get_global_type, symbols_, nodes_, scope_, env_);
}

}  // namespace bt_dsl
