#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bt_dsl/lsp.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace
{

using Workspace = bt_dsl::lsp::Workspace;

std::unordered_map<uint32_t, std::unique_ptr<Workspace>> g_workspaces;
uint32_t g_next_handle = 1;

char * dup_cstr(const std::string & s)
{
  // Host must free with bt_free.
  char * out = static_cast<char *>(std::malloc(s.size() + 1));
  if (!out) {
    return nullptr;
  }
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  return out;
}

std::vector<std::string> parse_import_uris_json(const char * imports_json)
{
  std::vector<std::string> out;
  if (!imports_json || !*imports_json) {
    return out;
  }
  try {
    const auto j = nlohmann::json::parse(imports_json);
    if (!j.is_array()) {
      return out;
    }
    for (const auto & v : j) {
      if (v.is_string()) {
        out.push_back(v.get<std::string>());
      }
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Ignore parse errors; treat as no imports.
  }
  return out;
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE uint32_t bt_workspace_create()
{
  const uint32_t h = g_next_handle++;
  g_workspaces[h] = std::make_unique<Workspace>();
  return h;
}

EMSCRIPTEN_KEEPALIVE void bt_workspace_destroy(uint32_t handle) { g_workspaces.erase(handle); }

EMSCRIPTEN_KEEPALIVE void bt_workspace_set_document(
  uint32_t handle, const char * uri, const char * text_utf8)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri || !text_utf8) {
    return;
  }
  it->second->set_document(std::string(uri), std::string(text_utf8));
}

EMSCRIPTEN_KEEPALIVE void bt_workspace_remove_document(uint32_t handle, const char * uri)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return;
  }
  it->second->remove_document(std::string_view(uri));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_diagnostics_json(uint32_t handle, const char * uri)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->diagnostics_json(std::string_view(uri)));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_diagnostics_json_with_imports(
  uint32_t handle, const char * uri, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(it->second->diagnostics_json(std::string_view(uri), imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_completion_json(
  uint32_t handle, const char * uri, uint32_t byte_offset)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->completion_json(std::string_view(uri), byte_offset));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_completion_json_with_imports(
  uint32_t handle, const char * uri, uint32_t byte_offset, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(it->second->completion_json(std::string_view(uri), byte_offset, imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_hover_json(
  uint32_t handle, const char * uri, uint32_t byte_offset)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->hover_json(std::string_view(uri), byte_offset));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_hover_json_with_imports(
  uint32_t handle, const char * uri, uint32_t byte_offset, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(it->second->hover_json(std::string_view(uri), byte_offset, imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_definition_json(
  uint32_t handle, const char * uri, uint32_t byte_offset)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->definition_json(std::string_view(uri), byte_offset));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_definition_json_with_imports(
  uint32_t handle, const char * uri, uint32_t byte_offset, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(it->second->definition_json(std::string_view(uri), byte_offset, imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_document_symbols_json(uint32_t handle, const char * uri)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->document_symbols_json(std::string_view(uri)));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_document_highlights_json(
  uint32_t handle, const char * uri, uint32_t byte_offset)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->document_highlights_json(std::string_view(uri), byte_offset));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_document_highlights_json_with_imports(
  uint32_t handle, const char * uri, uint32_t byte_offset, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(
    it->second->document_highlights_json(std::string_view(uri), byte_offset, imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_semantic_tokens_json(uint32_t handle, const char * uri)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  return dup_cstr(it->second->semantic_tokens_json(std::string_view(uri)));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_semantic_tokens_json_with_imports(
  uint32_t handle, const char * uri, const char * imports_json)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const auto imports = parse_import_uris_json(imports_json);
  return dup_cstr(it->second->semantic_tokens_json(std::string_view(uri), imports));
}

EMSCRIPTEN_KEEPALIVE char * bt_workspace_resolve_imports_json(
  uint32_t handle, const char * uri, const char * stdlib_uri)
{
  auto it = g_workspaces.find(handle);
  if (it == g_workspaces.end() || !uri) {
    return dup_cstr("{}");
  }
  const std::string_view stdlib =
    (stdlib_uri && *stdlib_uri) ? std::string_view(stdlib_uri) : std::string_view{};
  return dup_cstr(it->second->resolve_imports_json(std::string_view(uri), stdlib));
}

EMSCRIPTEN_KEEPALIVE void bt_free(char * ptr) { std::free(ptr); }

}  // extern "C"
