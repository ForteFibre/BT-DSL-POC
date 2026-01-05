// bt_dsl/basic/diagnostic.cpp - Diagnostic implementation
#include "bt_dsl/basic/diagnostic.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace bt_dsl
{

const Label * Diagnostic::primary_label() const noexcept
{
  for (const auto & l : labels) {
    if (l.style == LabelStyle::Primary) {
      return &l;
    }
  }
  if (!labels.empty()) {
    return &labels.front();
  }
  return nullptr;
}

SourceRange Diagnostic::primary_range() const noexcept
{
  const Label * l = primary_label();
  if (l == nullptr) {
    return {};
  }
  return l->range;
}

// ============================================================================
// DiagnosticBuilder
// ============================================================================

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBag & bag, Diagnostic diag)
: bag_(bag), diagnostic_(std::move(diag))
{
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder && other) noexcept
: bag_(other.bag_), diagnostic_(std::move(other.diagnostic_)), active_(other.active_)
{
  other.active_ = false;
}

DiagnosticBuilder::~DiagnosticBuilder()
{
  if (active_) {
    bag_.add(std::move(diagnostic_));
  }
}

DiagnosticBuilder & DiagnosticBuilder::with_code(std::string code)
{
  diagnostic_.code = std::move(code);
  return *this;
}

DiagnosticBuilder & DiagnosticBuilder::with_label(
  SourceRange range, std::string msg, LabelStyle style)
{
  diagnostic_.labels.push_back(Label{range, std::move(msg), style});
  return *this;
}

DiagnosticBuilder & DiagnosticBuilder::with_secondary_label(SourceRange range, std::string msg)
{
  return with_label(range, std::move(msg), LabelStyle::Secondary);
}

DiagnosticBuilder & DiagnosticBuilder::with_fixit(SourceRange range, std::string replacement)
{
  diagnostic_.fixits.push_back(FixIt{range, std::move(replacement)});
  return *this;
}

DiagnosticBuilder & DiagnosticBuilder::with_help(std::string help_msg)
{
  diagnostic_.help_message = std::move(help_msg);
  return *this;
}

// ============================================================================
// DiagnosticBag
// ============================================================================

DiagnosticBuilder DiagnosticBag::report_error(
  SourceRange range, std::string message, std::string label_message)
{
  Diagnostic d;
  d.severity = Severity::Error;
  d.message = std::move(message);
  d.labels.push_back(Label{range, std::move(label_message), LabelStyle::Primary});
  return {*this, std::move(d)};
}

DiagnosticBuilder DiagnosticBag::report_warning(
  SourceRange range, std::string message, std::string label_message)
{
  Diagnostic d;
  d.severity = Severity::Warning;
  d.message = std::move(message);
  d.labels.push_back(Label{range, std::move(label_message), LabelStyle::Primary});
  return {*this, std::move(d)};
}

DiagnosticBuilder DiagnosticBag::report_info(
  SourceRange range, std::string message, std::string label_message)
{
  Diagnostic d;
  d.severity = Severity::Info;
  d.message = std::move(message);
  d.labels.push_back(Label{range, std::move(label_message), LabelStyle::Primary});
  return {*this, std::move(d)};
}

DiagnosticBuilder DiagnosticBag::report_hint(
  SourceRange range, std::string message, std::string label_message)
{
  Diagnostic d;
  d.severity = Severity::Hint;
  d.message = std::move(message);
  d.labels.push_back(Label{range, std::move(label_message), LabelStyle::Primary});
  return {*this, std::move(d)};
}

void DiagnosticBag::add(Diagnostic && diag) { diagnostics_.push_back(std::move(diag)); }

void DiagnosticBag::add(const Diagnostic & diag) { diagnostics_.push_back(diag); }

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

void DiagnosticBag::merge(DiagnosticBag && other)
{
  diagnostics_.insert(
    diagnostics_.end(), std::make_move_iterator(other.diagnostics_.begin()),
    std::make_move_iterator(other.diagnostics_.end()));
  other.diagnostics_.clear();
}

void DiagnosticBag::merge(const DiagnosticBag & other)
{
  diagnostics_.insert(diagnostics_.end(), other.diagnostics_.begin(), other.diagnostics_.end());
}

}  // namespace bt_dsl
