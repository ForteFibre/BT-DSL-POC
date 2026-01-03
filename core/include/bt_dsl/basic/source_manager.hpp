// bt_dsl/basic/source_manager.hpp - Source location and range management
//
// This header provides types for tracking source code locations and ranges.
//
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bt_dsl
{

// ============================================================================
// SourceLocation - Compact source position
// ============================================================================

/**
 * A compact representation of a source location.
 *
 * Internally stores a byte offset into the source file. Line and column
 * information can be computed on demand via SourceManager.
 */
class SourceLocation
{
public:
  /// Invalid/unknown location sentinel
  static constexpr uint32_t k_invalid_offset = UINT32_MAX;

  /// Create an invalid location
  constexpr SourceLocation() noexcept : offset_(k_invalid_offset) {}

  /// Create a location from byte offset
  constexpr explicit SourceLocation(uint32_t offset) noexcept : offset_(offset) {}

  /// Check if this is a valid location
  [[nodiscard]] constexpr bool is_valid() const noexcept { return offset_ != k_invalid_offset; }

  /// Check if this is an invalid location
  [[nodiscard]] constexpr bool is_invalid() const noexcept { return offset_ == k_invalid_offset; }

  /// Get the byte offset
  [[nodiscard]] constexpr uint32_t get_offset() const noexcept { return offset_; }

  /// Comparison operators
  [[nodiscard]] constexpr bool operator==(SourceLocation other) const noexcept
  {
    return offset_ == other.offset_;
  }
  [[nodiscard]] constexpr bool operator!=(SourceLocation other) const noexcept
  {
    return offset_ != other.offset_;
  }
  [[nodiscard]] constexpr bool operator<(SourceLocation other) const noexcept
  {
    return offset_ < other.offset_;
  }
  [[nodiscard]] constexpr bool operator<=(SourceLocation other) const noexcept
  {
    return offset_ <= other.offset_;
  }
  [[nodiscard]] constexpr bool operator>(SourceLocation other) const noexcept
  {
    return offset_ > other.offset_;
  }
  [[nodiscard]] constexpr bool operator>=(SourceLocation other) const noexcept
  {
    return offset_ >= other.offset_;
  }

private:
  uint32_t offset_;
};

// ============================================================================
// SourceRange - Start and end locations
// ============================================================================

/**
 * A range of source code defined by start and end locations.
 *
 * The range is inclusive of the start and exclusive of the end,
 * following the half-open interval convention [start, end).
 */
class SourceRange
{
public:
  /// Create an invalid range
  constexpr SourceRange() noexcept = default;

  /// Create a range from start and end locations
  constexpr SourceRange(SourceLocation start, SourceLocation end) noexcept
  : start_(start), end_(end)
  {
  }

  /// Create a range from byte offsets
  constexpr SourceRange(uint32_t start_offset, uint32_t end_offset) noexcept
  : start_(SourceLocation(start_offset)), end_(SourceLocation(end_offset))
  {
  }

  /// Get the start location
  [[nodiscard]] constexpr SourceLocation get_begin() const noexcept { return start_; }

  /// Get the end location
  [[nodiscard]] constexpr SourceLocation get_end() const noexcept { return end_; }

  /// Check if this range is valid
  [[nodiscard]] constexpr bool is_valid() const noexcept
  {
    return start_.is_valid() && end_.is_valid();
  }

  /// Check if this range is invalid
  [[nodiscard]] constexpr bool is_invalid() const noexcept
  {
    return start_.is_invalid() || end_.is_invalid();
  }

  /// Check if a location is contained within this range
  [[nodiscard]] constexpr bool contains(SourceLocation loc) const noexcept
  {
    return loc >= start_ && loc < end_;
  }

  /// Check if another range is fully contained within this range
  [[nodiscard]] constexpr bool contains(SourceRange other) const noexcept
  {
    return other.start_ >= start_ && other.end_ <= end_;
  }

  /// Get the size in bytes
  [[nodiscard]] constexpr uint32_t size() const noexcept
  {
    if (is_invalid()) return 0;
    return end_.get_offset() - start_.get_offset();
  }

  /// Comparison operators
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

/**
 * Human-readable line and column position (1-indexed).
 */
struct LineColumn
{
  uint32_t line = 0;    ///< 1-indexed line number (0 = invalid)
  uint32_t column = 0;  ///< 1-indexed column number (0 = invalid)

  [[nodiscard]] constexpr bool is_valid() const noexcept { return line > 0 && column > 0; }
};

// ============================================================================
// FullSourceRange - Complete range with line/column info
// ============================================================================

/**
 * Extended source range including pre-computed line/column information.
 *
 * This is useful for error messages and diagnostics where line/column
 * information is frequently needed.
 */
struct FullSourceRange
{
  uint32_t start_line = 0;
  uint32_t start_column = 0;
  uint32_t end_line = 0;
  uint32_t end_column = 0;
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;

  /// Create from SourceRange (without line info - must be computed separately)
  static FullSourceRange from_byte_range(uint32_t start, uint32_t end)
  {
    FullSourceRange r;
    r.start_byte = start;
    r.end_byte = end;
    return r;
  }

  /// Convert to compact SourceRange
  [[nodiscard]] SourceRange to_source_range() const noexcept { return {start_byte, end_byte}; }

  /// Check if valid
  [[nodiscard]] bool is_valid() const noexcept { return start_line > 0; }
};

// ============================================================================
// SourceManager - Source file and location management
// ============================================================================

/**
 * Manages source code content and provides location services.
 *
 * Features:
 * - Stores source file path using std::filesystem::path
 * - Stores source file content
 * - Converts between byte offsets and line/column positions
 * - Pre-computes line start offsets for efficient lookup
 */
class SourceManager
{
public:
  SourceManager() = default;

  /// Initialize with source content only (no file path)
  explicit SourceManager(std::string source) : source_(std::move(source)) { build_line_table(); }

  /// Initialize with file path and source content
  SourceManager(std::filesystem::path filePath, std::string source)
  : file_path_(std::move(filePath)), source_(std::move(source))
  {
    build_line_table();
  }

  // ===========================================================================
  // File Path Accessors
  // ===========================================================================

  /// Set the file path
  void set_file_path(std::filesystem::path path) { file_path_ = std::move(path); }

  /// Get the file path
  [[nodiscard]] const std::filesystem::path & get_file_path() const noexcept { return file_path_; }

  /// Check if file path is set
  [[nodiscard]] bool has_file_path() const noexcept { return !file_path_.empty(); }

  /// Get the file name only (without directory)
  [[nodiscard]] std::string get_file_name() const { return file_path_.filename().string(); }

  /// Get the parent directory path
  [[nodiscard]] std::filesystem::path get_directory() const { return file_path_.parent_path(); }

  /// Resolve a relative path against this file's directory
  [[nodiscard]] std::filesystem::path resolve_relative(
    const std::filesystem::path & relativePath) const
  {
    if (relativePath.is_absolute()) {
      return relativePath;
    }
    return get_directory() / relativePath;
  }

  // ===========================================================================
  // Source Content Accessors
  // ===========================================================================

  /// Set/replace the source content
  void set_source(std::string source)
  {
    source_ = std::move(source);
    build_line_table();
  }

  /// Get the source content
  [[nodiscard]] std::string_view get_source() const noexcept { return source_; }

  /// Get source size in bytes
  [[nodiscard]] size_t size() const noexcept { return source_.size(); }

  /// Get the number of lines
  [[nodiscard]] size_t get_line_count() const noexcept { return line_offsets_.size(); }

  // ===========================================================================
  // Location Conversion
  // ===========================================================================

  /// Convert byte offset to line/column (1-indexed)
  [[nodiscard]] LineColumn get_line_column(uint32_t offset) const noexcept
  {
    return get_line_column(SourceLocation(offset));
  }

  /// Convert SourceLocation to line/column (1-indexed)
  [[nodiscard]] LineColumn get_line_column(SourceLocation loc) const noexcept;

  /// Get the byte offset of a line start (0-indexed line number)
  [[nodiscard]] uint32_t get_line_offset(uint32_t line_index) const noexcept
  {
    if (line_index >= line_offsets_.size()) {
      return static_cast<uint32_t>(source_.size());
    }
    return line_offsets_[line_index];
  }

  /// Get the content of a specific line (0-indexed)
  [[nodiscard]] std::string_view get_line(uint32_t line_index) const noexcept;

  /// Get a slice of source by range
  [[nodiscard]] std::string_view get_source_slice(SourceRange range) const noexcept
  {
    if (range.is_invalid()) return {};
    auto start = range.get_begin().get_offset();
    auto end = range.get_end().get_offset();
    if (start >= source_.size()) return {};
    if (end > source_.size()) end = static_cast<uint32_t>(source_.size());
    return std::string_view(source_).substr(start, end - start);
  }

  /// Expand a SourceRange to include full line/column info
  [[nodiscard]] FullSourceRange get_full_range(SourceRange range) const noexcept;

private:
  void build_line_table();

  std::filesystem::path file_path_;     ///< Path to the source file
  std::string source_;                  ///< Source content
  std::vector<uint32_t> line_offsets_;  ///< Offset of each line start
};

}  // namespace bt_dsl
