// bt_dsl/basic/diagnostic.hpp - Diagnostic types for parsing/sema
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

// ============================================================================
// Core Structures
// ============================================================================

/**
 * Severity level for diagnostics.
 */
enum class Severity : uint8_t {
  Error,
  Warning,
  Info,
  Hint,
};

enum class LabelStyle {
  Primary,    // エラーの直接的な原因
  Secondary,  // 関連情報（補足）
};

struct Label
{
  SourceRange range;
  std::string message;
  LabelStyle style = LabelStyle::Primary;
};

struct FixIt
{
  SourceRange range;
  std::string replacement_text;
};

struct Diagnostic
{
  Severity severity = Severity::Error;
  std::string code;     // e.g., "E042"
  std::string message;  // メインメッセージ

  std::vector<Label> labels;
  std::vector<FixIt> fixits;
  std::optional<std::string> help_message;

  [[nodiscard]] const Label * primary_label() const noexcept;
  [[nodiscard]] SourceRange primary_range() const noexcept;
};

// ============================================================================
// Forward Declarations
// ============================================================================

class DiagnosticBag;

// ============================================================================
// DiagnosticBuilder
// ============================================================================

/**
 * Fluent Interfaceでエラーを構築し、デストラクタで自動的にBagへ登録します（RAII）。
 */
class DiagnosticBuilder
{
public:
  DiagnosticBuilder(DiagnosticBag & bag, Diagnostic diag);

  DiagnosticBuilder(const DiagnosticBuilder &) = delete;
  DiagnosticBuilder & operator=(const DiagnosticBuilder &) = delete;

  DiagnosticBuilder(DiagnosticBuilder && other) noexcept;

  ~DiagnosticBuilder();

  DiagnosticBuilder & with_code(std::string code);

  DiagnosticBuilder & with_label(
    SourceRange range, std::string msg, LabelStyle style = LabelStyle::Primary);

  DiagnosticBuilder & with_secondary_label(SourceRange range, std::string msg);

  DiagnosticBuilder & with_fixit(SourceRange range, std::string replacement);

  DiagnosticBuilder & with_help(std::string help_msg);

private:
  DiagnosticBag & bag_;
  Diagnostic diagnostic_;
  bool active_ = true;
};

// ============================================================================
// DiagnosticBag
// ============================================================================

class DiagnosticBag
{
public:
  DiagnosticBag() = default;

  DiagnosticBag(const DiagnosticBag &) = default;
  DiagnosticBag & operator=(const DiagnosticBag &) = default;
  DiagnosticBag(DiagnosticBag &&) = default;
  DiagnosticBag & operator=(DiagnosticBag &&) = default;

  // Builder Starters
  DiagnosticBuilder report_error(
    SourceRange range, std::string message, std::string label_message = "");
  DiagnosticBuilder report_warning(
    SourceRange range, std::string message, std::string label_message = "");
  DiagnosticBuilder report_info(
    SourceRange range, std::string message, std::string label_message = "");
  DiagnosticBuilder report_hint(
    SourceRange range, std::string message, std::string label_message = "");

  // Add
  void add(Diagnostic && diag);
  void add(const Diagnostic & diag);

  // Accessors
  [[nodiscard]] const std::vector<Diagnostic> & all() const { return diagnostics_; }
  [[nodiscard]] bool empty() const { return diagnostics_.empty(); }
  [[nodiscard]] size_t size() const { return diagnostics_.size(); }

  [[nodiscard]] std::vector<Diagnostic> errors() const;
  [[nodiscard]] std::vector<Diagnostic> warnings() const;
  [[nodiscard]] bool has_errors() const;
  [[nodiscard]] bool has_warnings() const;

  // Utilities
  void merge(DiagnosticBag && other);
  void merge(const DiagnosticBag & other);

  [[nodiscard]] auto begin() const { return diagnostics_.begin(); }
  [[nodiscard]] auto end() const { return diagnostics_.end(); }

private:
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace bt_dsl
