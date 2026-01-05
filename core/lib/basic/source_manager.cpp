// bt_dsl/basic/source_manager.cpp - Source file and registry implementation
#include "bt_dsl/basic/source_manager.hpp"

#include <algorithm>

namespace bt_dsl
{

// ============================================================================
// SourceFile
// ============================================================================

SourceFile::SourceFile(fs::path path, std::string content)
: path_(std::move(path)), content_(std::move(content))
{
  build_line_table();
}

void SourceFile::set_content(std::string new_content)
{
  content_ = std::move(new_content);
  build_line_table();
}

LineColumn SourceFile::get_line_column(uint32_t offset) const noexcept
{
  if (line_offsets_.empty()) {
    return {};
  }

  if (offset > content_.size()) {
    offset = static_cast<uint32_t>(content_.size());
  }

  auto it = std::upper_bound(line_offsets_.begin(), line_offsets_.end(), offset);
  if (it == line_offsets_.begin()) {
    return {1, offset + 1};
  }
  --it;

  const uint32_t line = static_cast<uint32_t>(it - line_offsets_.begin()) + 1;
  const uint32_t column = offset - *it + 1;
  return {line, column};
}

std::string_view SourceFile::get_line(uint32_t line_index) const noexcept
{
  if (line_index >= line_offsets_.size()) {
    return {};
  }

  const uint32_t start = line_offsets_[line_index];
  auto end = static_cast<uint32_t>(content_.size());
  if (line_index + 1 < line_offsets_.size()) {
    end = line_offsets_[line_index + 1];
    if (end > start && content_[end - 1] == '\n') {
      --end;
    }
  }

  return std::string_view(content_).substr(start, end - start);
}

std::string_view SourceFile::get_slice(SourceRange range) const noexcept
{
  if (!range.is_valid()) {
    return {};
  }

  const auto start = range.get_begin().offset();
  auto end = range.get_end().offset();
  if (start >= content_.size()) {
    return {};
  }
  if (end > content_.size()) {
    end = static_cast<uint32_t>(content_.size());
  }
  return std::string_view(content_).substr(start, end - start);
}

FullSourceRange SourceFile::get_full_range(SourceRange range) const noexcept
{
  FullSourceRange result;
  if (!range.is_valid()) {
    return result;
  }

  result.start_byte = range.get_begin().offset();
  result.end_byte = range.get_end().offset();

  const auto start_lc = get_line_column(result.start_byte);
  const auto end_lc = get_line_column(result.end_byte);

  result.start_line = start_lc.line;
  result.start_column = start_lc.column;
  result.end_line = end_lc.line;
  result.end_column = end_lc.column;

  return result;
}

void SourceFile::build_line_table()
{
  line_offsets_.clear();
  line_offsets_.push_back(0);

  for (size_t i = 0; i < content_.size(); ++i) {
    if (content_[i] == '\n') {
      line_offsets_.push_back(static_cast<uint32_t>(i + 1));
    }
  }
}

// ============================================================================
// SourceRegistry
// ============================================================================

std::string SourceRegistry::normalize_key(const fs::path & path)
{
  try {
    return fs::weakly_canonical(path).string();
  } catch (const fs::filesystem_error &) {
    return path.lexically_normal().string();
  }
}

FileId SourceRegistry::register_file(fs::path path, std::string content)
{
  const std::string key = normalize_key(path);
  if (const auto it = path_to_id_.find(key); it != path_to_id_.end()) {
    return it->second;
  }

  if (files_.size() >= static_cast<size_t>(FileId::k_invalid)) {
    return FileId::invalid();
  }

  const FileId id{static_cast<uint16_t>(files_.size())};
  files_.push_back(std::make_unique<SourceFile>(std::move(path), std::move(content)));
  path_to_id_.emplace(key, id);
  return id;
}

void SourceRegistry::update_content(FileId id, std::string new_content)
{
  if (!id.is_valid()) {
    return;
  }
  const auto idx = static_cast<size_t>(id.value);
  if (idx >= files_.size() || files_[idx] == nullptr) {
    return;
  }
  files_[idx]->set_content(std::move(new_content));
}

const SourceFile * SourceRegistry::get_file(FileId id) const noexcept
{
  if (!id.is_valid()) {
    return nullptr;
  }
  const auto idx = static_cast<size_t>(id.value);
  if (idx >= files_.size()) {
    return nullptr;
  }
  return files_[idx].get();
}

const fs::path & SourceRegistry::get_path(FileId id) const noexcept
{
  static const fs::path k_empty;
  const auto * f = get_file(id);
  return f ? f->path() : k_empty;
}

std::optional<FileId> SourceRegistry::find_by_path(const fs::path & path) const
{
  const std::string key = normalize_key(path);
  if (const auto it = path_to_id_.find(key); it != path_to_id_.end()) {
    return it->second;
  }
  return std::nullopt;
}

LineColumn SourceRegistry::get_line_column(SourceLocation loc) const noexcept
{
  const auto * f = get_file(loc.file_id());
  if (f == nullptr || !loc.is_valid()) {
    return {};
  }
  return f->get_line_column(loc.offset());
}

FullSourceRange SourceRegistry::get_full_range(SourceRange range) const noexcept
{
  const auto * f = get_file(range.file_id());
  if (f == nullptr) {
    return {};
  }
  return f->get_full_range(range);
}

std::string_view SourceRegistry::get_slice(SourceRange range) const noexcept
{
  const auto * f = get_file(range.file_id());
  if (f == nullptr) {
    return {};
  }
  return f->get_slice(range);
}

}  // namespace bt_dsl
