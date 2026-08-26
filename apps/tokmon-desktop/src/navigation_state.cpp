#include "navigation_state.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "platform_utils.hpp"

namespace tokmon::desktop {

void refresh_navigation(
    const std::shared_ptr<slint::VectorModel<NavigationItem>> &model,
    const std::shared_ptr<std::vector<NavigationItem>> &items,
    std::string query, const slint::ComponentWeakHandle<MainWindow> &window) {
  for (auto &character : query)
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

  const auto matches = [&query](const NavigationItem &item) {
    auto title = std::string(item.title);
    for (auto &character : title)
      character = static_cast<char>(
          std::tolower(static_cast<unsigned char>(character)));
    return title.find(query) != std::string::npos;
  };

  int visible_sessions = 0;
  model->clear();
  for (std::size_t row = 0; row < items->size(); ++row) {
    bool shown = true;
    if (query.empty()) {
      auto required_indent = (*items)[row].indent - 1;
      for (std::size_t previous = row; previous > 0 && required_indent >= 0;) {
        --previous;
        if ((*items)[previous].indent != required_indent)
          continue;
        if (!(*items)[previous].expanded)
          shown = false;
        --required_indent;
      }
    } else {
      shown = matches((*items)[row]);
      for (std::size_t child = row + 1;
           !shown && child < items->size() &&
           (*items)[child].indent > (*items)[row].indent;
           ++child)
        shown = matches((*items)[child]);
    }
    if (shown) {
      model->push_back((*items)[row]);
      if ((*items)[row].kind == "session")
        ++visible_sessions;
    }
  }
  if (window.lock()) {
    auto weak = window;
    const auto count = query.empty() ? 0 : visible_sessions;
    (void)slint::invoke_from_event_loop([weak, count] {
      if (auto locked = weak.lock())
        (*locked)->set_search_session_count(count);
    });
  }
}

std::string short_workspace_label(const std::filesystem::path &workspace,
                                  const std::filesystem::path &root_workspace) {
  auto text = path_to_utf8(workspace);
  auto root_text = path_to_utf8(root_workspace);
  if (!root_text.empty()) {
    auto prefix = root_text;
    while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
      prefix.pop_back();
    if (text.size() > prefix.size() && text.starts_with(prefix)) {
      auto rest = text.substr(prefix.size());
      while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\'))
        rest.erase(rest.begin());
      if (rest.empty())
        return "~/";
      return "~/" + rest;
    }
  }
  return text;
}

std::string git_branch_label(const std::filesystem::path &workspace) {
  std::error_code error;
  const auto head = workspace / ".git" / "HEAD";
  if (!std::filesystem::is_regular_file(head, error))
    return {};
  std::ifstream stream(head, std::ios::binary);
  if (!stream)
    return {};
  std::string line;
  std::getline(stream, line);
  const auto prefix = std::string_view("ref: refs/heads/");
  if (line.starts_with(prefix))
    return std::string(line.substr(prefix.size()));
  if (line.size() >= 12)
    return line.substr(0, 12);
  return {};
}

int count_indexed_files(const std::filesystem::path &workspace) {
  int files = 0;
  std::error_code error;
  std::function<void(const std::filesystem::path &, int)> scan =
      [&](const std::filesystem::path &directory, int depth) {
        if (depth > 3 || files > 4'096)
          return;
        for (std::filesystem::directory_iterator it(directory, error), end;
             it != end && !error; it.increment(error)) {
          const auto &entry = *it;
          std::error_code entry_error;
          if (entry.is_directory(entry_error)) {
            const auto name = entry.path().filename().string();
            if (name == ".git" || name == "node_modules" || name == ".tokmon" ||
                name == "build")
              continue;
            scan(entry.path(), depth + 1);
          } else if (entry.is_regular_file(entry_error)) {
            ++files;
            if (files > 4'096)
              return;
          }
        }
      };
  scan(workspace, 1);
  return files;
}

NavigationItem make_navigation_item(const std::filesystem::path &assets,
                                    std::string id, std::string kind,
                                    std::string title, const int indent,
                                    const bool selected, const bool expanded,
                                    std::string ray, std::string workspace,
                                    const bool title_manual) {
  NavigationItem item;
  item.id = display_string(id);
  item.ray = display_string(ray);
  item.workspace = display_string(workspace);
  item.kind = display_string(kind);
  item.title = display_string(title);
  item.title_manual = title_manual;
  item.indent = indent;
  item.selected = selected;
  item.expandable = kind != "session";
  item.expanded = expanded;
  const auto icon = kind == "group"     ? "icon-06.svg"
                    : kind == "project" ? "icon-08.svg"
                                        : "icon-09.svg";
  item.icon = slint::Image::load_from_path(
      slint::SharedString((assets / icon).string()));
  return item;
}

tokmon::cbor::Value navigation_value(const std::vector<NavigationItem> &items) {
  tokmon::cbor::Value::Array encoded;
  encoded.reserve(items.size());
  for (const auto &item : items)
    encoded.push_back(tokmon::cbor::object(
        {{"id", std::string(item.id)},
         {"ray", std::string(item.ray)},
         {"workspace", std::string(item.workspace)},
         {"kind", std::string(item.kind)},
         {"title", std::string(item.title)},
         {"title-manual", item.title_manual},
         {"indent", static_cast<std::int64_t>(item.indent)},
         {"selected", item.selected},
         {"expanded", item.expanded}}));
  return encoded;
}

std::optional<std::vector<NavigationItem>>
navigation_items(const tokmon::cbor::Value &value,
                 const std::filesystem::path &assets,
                 const std::filesystem::path &default_workspace) {
  if (!value.as_array())
    return std::nullopt;
  std::vector<NavigationItem> items;
  items.reserve(value.as_array()->size());
  for (const auto &encoded : *value.as_array()) {
    const auto *id = tokmon::cbor::find(encoded, "id");
    const auto *kind = tokmon::cbor::find(encoded, "kind");
    const auto *title = tokmon::cbor::find(encoded, "title");
    const auto kind_text =
        kind ? std::string(kind->as_string()) : std::string{};
    if (!id || !title ||
        (kind_text != "group" && kind_text != "project" &&
         kind_text != "session"))
      return std::nullopt;
    const auto indent =
        tokmon::cbor::find(encoded, "indent")
            ? static_cast<int>(
                  tokmon::cbor::find(encoded, "indent")->as_integer())
            : 0;
    if (indent < 0 || indent > 8 || title->as_string().empty() ||
        title->as_string().size() > 256)
      return std::nullopt;
    std::string workspace;
    if (const auto *encoded_workspace =
            tokmon::cbor::find(encoded, "workspace")) {
      if (!std::holds_alternative<std::string>(encoded_workspace->data) ||
          encoded_workspace->as_string().size() > 4'096u ||
          encoded_workspace->as_string().find('\0') != std::string_view::npos)
        return std::nullopt;
      workspace = std::string(encoded_workspace->as_string());
    }
    if (kind_text == "group") {
      workspace.clear();
    } else if (kind_text == "project" && workspace.empty()) {
      workspace = path_to_utf8(default_workspace);
    }
    if (!workspace.empty()) {
      auto normalized = normalize_workspace_path(workspace, default_workspace);
      if (!normalized)
        return std::nullopt;
      workspace = path_to_utf8(*normalized);
    }
    items.push_back(make_navigation_item(
        assets, std::string(id->as_string()), kind_text,
        std::string(title->as_string()), indent,
        tokmon::cbor::find(encoded, "selected") &&
            tokmon::cbor::find(encoded, "selected")->as_bool(),
        !tokmon::cbor::find(encoded, "expanded") ||
            tokmon::cbor::find(encoded, "expanded")->as_bool(),
        tokmon::cbor::find(encoded, "ray")
            ? std::string(tokmon::cbor::find(encoded, "ray")->as_string())
            : std::string{},
        std::move(workspace),
        // Older navigation files predate this field. Treat their existing
        // titles as intentional so an old empty conversation is never renamed
        // just because it is opened after upgrading.
        !tokmon::cbor::find(encoded, "title-manual") ||
            !std::holds_alternative<bool>(
                tokmon::cbor::find(encoded, "title-manual")->data) ||
            tokmon::cbor::find(encoded, "title-manual")->as_bool()));
  }
  return items;
}

std::filesystem::path
navigation_workspace_at(const std::vector<NavigationItem> &items,
                        const std::size_t index,
                        const std::filesystem::path &fallback) {
  if (index >= items.size())
    return fallback;
  if (!std::string(items[index].workspace).empty()) {
    if (auto normalized = normalize_workspace_path(
            std::string(items[index].workspace), fallback))
      return *normalized;
  }
  for (std::size_t previous = index; previous > 0;) {
    --previous;
    if (items[previous].indent >= items[index].indent)
      continue;
    if (items[previous].kind == "project") {
      if (auto normalized = normalize_workspace_path(
              std::string(items[previous].workspace), fallback))
        return *normalized;
    }
    break;
  }
  return fallback;
}

std::size_t navigation_ancestor_at(const std::vector<NavigationItem> &items,
                                   const std::size_t index,
                                   const std::string_view kind) {
  if (index >= items.size())
    return items.size();
  if (std::string(items[index].kind) == kind)
    return index;
  auto ancestor_indent = items[index].indent;
  for (std::size_t previous = index; previous > 0;) {
    --previous;
    if (items[previous].indent >= ancestor_indent)
      continue;
    ancestor_indent = items[previous].indent;
    if (std::string(items[previous].kind) == kind)
      return previous;
  }
  return items.size();
}

} // namespace tokmon::desktop
