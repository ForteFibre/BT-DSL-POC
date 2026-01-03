// bt_dsl/basic/source_manager.cpp - Source location implementation
//
#include "bt_dsl/basic/source_manager.hpp"

#include <algorithm>

namespace bt_dsl
{

LineColumn SourceManager::get_line_column(SourceLocation loc) const noexcept
{
  if (loc.is_invalid() || line_offsets_.empty()) {
    return {};
  }

  uint32_t offset = loc.get_offset();
  if (offset > source_.size()) {
    offset = static_cast<uint32_t>(source_.size());
  }

  // Binary search for the line
  auto it = std::upper_bound(line_offsets_.begin(), line_offsets_.end(), offset);
  if (it == line_offsets_.begin()) {
    return {1, offset + 1};
  }
  --it;

  const uint32_t line = static_cast<uint32_t>(it - line_offsets_.begin()) + 1;
  const uint32_t column = offset - *it + 1;
  return {line, column};
}

std::string_view SourceManager::get_line(uint32_t line_index) const noexcept
{
  if (line_index >= line_offsets_.size()) {
    return {};
  }

  const uint32_t start = line_offsets_[line_index];
  auto end = static_cast<uint32_t>(source_.size());
  if (line_index + 1 < line_offsets_.size()) {
    end = line_offsets_[line_index + 1];
    // Remove trailing newline
    if (end > start && source_[end - 1] == '\n') {
      --end;
    }
  }

  return std::string_view(source_).substr(start, end - start);
}

FullSourceRange SourceManager::get_full_range(SourceRange range) const noexcept
{
  FullSourceRange result;
  result.start_byte = range.get_begin().get_offset();
  result.end_byte = range.get_end().get_offset();

  const auto start_lc = get_line_column(range.get_begin());
  const auto end_lc = get_line_column(range.get_end());

  result.start_line = start_lc.line;
  result.start_column = start_lc.column;
  result.end_line = end_lc.line;
  result.end_column = end_lc.column;

  return result;
}

void SourceManager::build_line_table()
{
  line_offsets_.clear();
  line_offsets_.push_back(0);  // First line starts at offset 0

  for (size_t i = 0; i < source_.size(); ++i) {
    if (source_[i] == '\n') {
      line_offsets_.push_back(static_cast<uint32_t>(i + 1));
    }
  }
}

}  // namespace bt_dsl
