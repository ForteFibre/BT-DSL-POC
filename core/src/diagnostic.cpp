// bt_dsl/diagnostic.cpp - Diagnostic implementation
#include "bt_dsl/diagnostic.hpp"

#include <algorithm>

namespace bt_dsl
{

// ============================================================================
// Diagnostic Factory Methods
// ============================================================================

Diagnostic Diagnostic::error(SourceRange range, std::string message, std::string code)
{
  return Diagnostic{std::move(message), range, Severity::Error, std::move(code)};
}

Diagnostic Diagnostic::warning(SourceRange range, std::string message, std::string code)
{
  return Diagnostic{std::move(message), range, Severity::Warning, std::move(code)};
}

Diagnostic Diagnostic::info(SourceRange range, std::string message, std::string code)
{
  return Diagnostic{std::move(message), range, Severity::Info, std::move(code)};
}

Diagnostic Diagnostic::hint(SourceRange range, std::string message, std::string code)
{
  return Diagnostic{std::move(message), range, Severity::Hint, std::move(code)};
}

// ============================================================================
// DiagnosticBag Implementation
// ============================================================================

void DiagnosticBag::add(Diagnostic diag) { diagnostics_.push_back(std::move(diag)); }

void DiagnosticBag::error(SourceRange range, std::string_view message, std::string_view code)
{
  diagnostics_.push_back(Diagnostic::error(range, std::string(message), std::string(code)));
}

void DiagnosticBag::warning(SourceRange range, std::string_view message, std::string_view code)
{
  diagnostics_.push_back(Diagnostic::warning(range, std::string(message), std::string(code)));
}

void DiagnosticBag::info(SourceRange range, std::string_view message, std::string_view code)
{
  diagnostics_.push_back(Diagnostic::info(range, std::string(message), std::string(code)));
}

void DiagnosticBag::hint(SourceRange range, std::string_view message, std::string_view code)
{
  diagnostics_.push_back(Diagnostic::hint(range, std::string(message), std::string(code)));
}

bool DiagnosticBag::has_errors() const
{
  return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic & d) {
    return d.severity == Severity::Error;
  });
}

bool DiagnosticBag::has_warnings() const
{
  return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic & d) {
    return d.severity == Severity::Warning;
  });
}

bool DiagnosticBag::empty() const { return diagnostics_.empty(); }

size_t DiagnosticBag::size() const { return diagnostics_.size(); }

const std::vector<Diagnostic> & DiagnosticBag::all() const { return diagnostics_; }

std::vector<Diagnostic> DiagnosticBag::errors() const
{
  std::vector<Diagnostic> result;
  std::copy_if(
    diagnostics_.begin(), diagnostics_.end(), std::back_inserter(result),
    [](const Diagnostic & d) { return d.severity == Severity::Error; });
  return result;
}

std::vector<Diagnostic> DiagnosticBag::warnings() const
{
  std::vector<Diagnostic> result;
  std::copy_if(
    diagnostics_.begin(), diagnostics_.end(), std::back_inserter(result),
    [](const Diagnostic & d) { return d.severity == Severity::Warning; });
  return result;
}

void DiagnosticBag::merge(const DiagnosticBag & other)
{
  diagnostics_.insert(diagnostics_.end(), other.diagnostics_.begin(), other.diagnostics_.end());
}

void DiagnosticBag::merge(DiagnosticBag && other)
{
  diagnostics_.insert(
    diagnostics_.end(), std::make_move_iterator(other.diagnostics_.begin()),
    std::make_move_iterator(other.diagnostics_.end()));
}

}  // namespace bt_dsl
