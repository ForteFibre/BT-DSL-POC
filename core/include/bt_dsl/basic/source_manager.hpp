// bt_dsl/basic/source_manager.hpp - Source location and file registry
//
// This header provides types for tracking source code locations and ranges,
// plus a central registry (Single Source of Truth) for source files.
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bt_dsl
{

namespace fs = std::filesystem;

// ============================================================================
// FileId - ファイル識別子（16bit = 65535ファイル）
// ============================================================================

struct FileId
{
  static constexpr uint16_t k_invalid = UINT16_MAX;
  uint16_t value = k_invalid;

  [[nodiscard]] constexpr bool is_valid() const noexcept { return value != k_invalid; }
  [[nodiscard]] static constexpr FileId invalid() noexcept { return {}; }

  [[nodiscard]] constexpr bool operator==(FileId other) const noexcept
  {
    return value == other.value;
  }
  [[nodiscard]] constexpr bool operator!=(FileId other) const noexcept
  {
    return value != other.value;
  }
  [[nodiscard]] constexpr bool operator<(FileId other) const noexcept
  {
    return value < other.value;
  }
};

// ============================================================================
// SourceLocation - 完全な位置情報（FileId + offset）
// ============================================================================

class SourceLocation
{
public:
  static constexpr uint32_t k_invalid_offset = UINT32_MAX;

  constexpr SourceLocation() noexcept = default;
  constexpr SourceLocation(FileId file, uint32_t offset) noexcept : offset_(offset), file_id_(file)
  {
  }

  [[nodiscard]] constexpr bool is_valid() const noexcept
  {
    return offset_ != k_invalid_offset && file_id_.is_valid();
  }
  [[nodiscard]] constexpr bool is_invalid() const noexcept { return !is_valid(); }

  [[nodiscard]] constexpr FileId file_id() const noexcept { return file_id_; }
  [[nodiscard]] constexpr uint32_t offset() const noexcept { return offset_; }

  // Legacy-style accessor used in existing call sites; prefer offset().
  [[nodiscard]] constexpr uint32_t get_offset() const noexcept { return offset(); }

  [[nodiscard]] constexpr bool operator==(SourceLocation other) const noexcept
  {
    return offset_ == other.offset_ && file_id_ == other.file_id_;
  }
  [[nodiscard]] constexpr bool operator!=(SourceLocation other) const noexcept
  {
    return !(*this == other);
  }

  [[nodiscard]] constexpr bool operator<(SourceLocation other) const noexcept
  {
    if (file_id_ != other.file_id_) {
      return file_id_ < other.file_id_;
    }
    return offset_ < other.offset_;
  }
  [[nodiscard]] constexpr bool operator<=(SourceLocation other) const noexcept
  {
    return *this < other || *this == other;
  }
  [[nodiscard]] constexpr bool operator>(SourceLocation other) const noexcept
  {
    return other < *this;
  }
  [[nodiscard]] constexpr bool operator>=(SourceLocation other) const noexcept
  {
    return other <= *this;
  }

private:
  uint32_t offset_ = k_invalid_offset;
  FileId file_id_;
};

// ============================================================================
// SourceRange - Start and end locations
// ============================================================================

class SourceRange
{
public:
  constexpr SourceRange() noexcept = default;

  constexpr SourceRange(SourceLocation start, SourceLocation end) noexcept
  : start_(start), end_(end)
  {
  }

  constexpr SourceRange(FileId file, uint32_t start_offset, uint32_t end_offset) noexcept
  : start_(SourceLocation(file, start_offset)), end_(SourceLocation(file, end_offset))
  {
  }

  [[nodiscard]] constexpr SourceLocation get_begin() const noexcept { return start_; }
  [[nodiscard]] constexpr SourceLocation get_end() const noexcept { return end_; }

  [[nodiscard]] constexpr FileId file_id() const noexcept
  {
    if (!start_.is_valid() || !end_.is_valid()) {
      return FileId::invalid();
    }
    if (start_.file_id() != end_.file_id()) {
      return FileId::invalid();
    }
    return start_.file_id();
  }

  [[nodiscard]] constexpr bool is_valid() const noexcept
  {
    return start_.is_valid() && end_.is_valid() && start_.file_id() == end_.file_id();
  }
  [[nodiscard]] constexpr bool is_invalid() const noexcept { return !is_valid(); }

  [[nodiscard]] constexpr bool contains(SourceLocation loc) const noexcept
  {
    if (!is_valid() || !loc.is_valid()) {
      return false;
    }
    if (loc.file_id() != start_.file_id()) {
      return false;
    }
    return loc >= start_ && loc < end_;
  }

  [[nodiscard]] constexpr bool contains(SourceRange other) const noexcept
  {
    if (!is_valid() || !other.is_valid()) {
      return false;
    }
    if (file_id() != other.file_id()) {
      return false;
    }
    return other.start_ >= start_ && other.end_ <= end_;
  }

  [[nodiscard]] constexpr uint32_t size() const noexcept
  {
    if (is_invalid()) return 0;
    return end_.offset() - start_.offset();
  }

  [[nodiscard]] constexpr bool operator==(SourceRange other) const noexcept
  {
    return start_ == other.start_ && end_ == other.end_;
  }
  [[nodiscard]] constexpr bool operator!=(SourceRange other) const noexcept
  {
    return !(*this == other);
  }

private:
  SourceLocation start_;
  SourceLocation end_;
};

// ============================================================================
// LineColumn - Human-readable position
// ============================================================================

struct LineColumn
{
  uint32_t line = 0;    // 1-indexed line number (0 = invalid)
  uint32_t column = 0;  // 1-indexed column number (0 = invalid)

  [[nodiscard]] constexpr bool is_valid() const noexcept { return line > 0 && column > 0; }
};

// ============================================================================
// FullSourceRange - Complete range with line/column info
// ============================================================================

struct FullSourceRange
{
  uint32_t start_line = 0;
  uint32_t start_column = 0;
  uint32_t end_line = 0;
  uint32_t end_column = 0;
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;

  static FullSourceRange from_byte_range(uint32_t start, uint32_t end)
  {
    FullSourceRange r;
    r.start_byte = start;
    r.end_byte = end;
    return r;
  }

  [[nodiscard]] SourceRange to_source_range(FileId file) const noexcept
  {
    return {file, start_byte, end_byte};
  }

  [[nodiscard]] bool is_valid() const noexcept { return start_line > 0; }
};

// ============================================================================
// SourceFile - 1ファイルの情報
// ============================================================================

class SourceFile
{
public:
  SourceFile() = default;
  SourceFile(fs::path path, std::string content);

  [[nodiscard]] const fs::path & path() const noexcept { return path_; }
  [[nodiscard]] std::string_view content() const noexcept { return content_; }
  [[nodiscard]] size_t size() const noexcept { return content_.size(); }
  [[nodiscard]] size_t line_count() const noexcept { return line_offsets_.size(); }

  [[nodiscard]] LineColumn get_line_column(uint32_t offset) const noexcept;
  [[nodiscard]] std::string_view get_line(uint32_t line_index) const noexcept;
  [[nodiscard]] std::string_view get_slice(SourceRange range) const noexcept;
  [[nodiscard]] FullSourceRange get_full_range(SourceRange range) const noexcept;

  void set_content(std::string new_content);

private:
  void build_line_table();

  fs::path path_;
  std::string content_;
  std::vector<uint32_t> line_offsets_;
};

// ============================================================================
// SourceRegistry - 全ファイルの中央管理
// ============================================================================

class SourceRegistry
{
public:
  FileId register_file(fs::path path, std::string content);
  void update_content(FileId id, std::string new_content);

  [[nodiscard]] const SourceFile * get_file(FileId id) const noexcept;
  [[nodiscard]] const fs::path & get_path(FileId id) const noexcept;

  [[nodiscard]] std::optional<FileId> find_by_path(const fs::path & path) const;

  [[nodiscard]] LineColumn get_line_column(SourceLocation loc) const noexcept;
  [[nodiscard]] FullSourceRange get_full_range(SourceRange range) const noexcept;
  [[nodiscard]] std::string_view get_slice(SourceRange range) const noexcept;

private:
  [[nodiscard]] static std::string normalize_key(const fs::path & path);

  std::vector<std::unique_ptr<SourceFile>> files_;
  std::unordered_map<std::string, FileId> path_to_id_;
};

}  // namespace bt_dsl
