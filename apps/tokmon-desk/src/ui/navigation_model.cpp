#include "ui/navigation_model.hpp"

#include "tokmon/ids.hpp"

#include <algorithm>
#include <cctype>

namespace tokmon::desk {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool valid_kind(const std::string_view value) {
  return value == "group" || value == "project" || value == "session";
}

std::filesystem::path normalized(const std::filesystem::path& value,
                                 const std::filesystem::path& fallback) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(
      value.empty() ? fallback : value, error);
  if (error)
    result = std::filesystem::absolute(
        value.empty() ? fallback : value, error).lexically_normal();
  return error ? fallback : result;
}

} // namespace

NavigationModel::NavigationModel(std::filesystem::path default_workspace)
    : default_workspace_(normalized(default_workspace, default_workspace)) {}

bool NavigationModel::load(const tokmon::cbor::Value& encoded,
                           std::string& error) {
  if (!encoded.as_array()) {
    error = "navigation must be an array";
    return false;
  }
  std::vector<DeskNavigationItem> decoded;
  decoded.reserve(encoded.as_array()->size());
  for (const auto& value : *encoded.as_array()) {
    const auto* id = tokmon::cbor::find(value, "id");
    const auto* kind = tokmon::cbor::find(value, "kind");
    const auto* title = tokmon::cbor::find(value, "title");
    if (!id || !kind || !title || id->as_string().empty() ||
        !valid_kind(kind->as_string()) || title->as_string().empty() ||
        title->as_string().size() > 256) {
      error = "navigation item is invalid";
      return false;
    }
    const auto indent_value = tokmon::cbor::find(value, "indent")
        ? tokmon::cbor::find(value, "indent")->as_integer() : 0;
    if (indent_value < 0 || indent_value > 8) {
      error = "navigation indent is invalid";
      return false;
    }
    std::filesystem::path workspace;
    if (const auto* field = tokmon::cbor::find(value, "workspace");
        field && !field->as_string().empty()) {
      if (field->as_string().size() > 4096 ||
          field->as_string().find('\0') != std::string_view::npos) {
        error = "navigation workspace is invalid";
        return false;
      }
      workspace = normalized(std::filesystem::path(
          std::string(field->as_string())), default_workspace_);
    }
    decoded.push_back({
        .id = std::string(id->as_string()),
        .ray = tokmon::cbor::find(value, "ray")
            ? std::string(tokmon::cbor::find(value, "ray")->as_string())
            : std::string{},
        .workspace = std::move(workspace),
        .kind = std::string(kind->as_string()),
        .title = std::string(title->as_string()),
        .indent = static_cast<int>(indent_value),
        .selected = tokmon::cbor::find(value, "selected") &&
                    tokmon::cbor::find(value, "selected")->as_bool(),
        .expanded = !tokmon::cbor::find(value, "expanded") ||
                    tokmon::cbor::find(value, "expanded")->as_bool(),
        .title_manual = !tokmon::cbor::find(value, "title-manual") ||
                        tokmon::cbor::find(value, "title-manual")->as_bool(),
    });
  }
  bool selected_seen = false;
  for (auto& item : decoded) {
    if (!item.selected)
      continue;
    item.selected = !selected_seen;
    selected_seen = true;
  }
  items_ = std::move(decoded);
  return true;
}

tokmon::cbor::Value NavigationModel::encode() const {
  tokmon::cbor::Value::Array values;
  values.reserve(items_.size());
  for (const auto& item : items_)
    values.push_back(tokmon::cbor::object({
        {"id", item.id}, {"ray", item.ray},
        {"workspace", item.workspace.empty() ? std::string{}
                                               : item.workspace.generic_string()},
        {"kind", item.kind}, {"title", item.title},
        {"title-manual", item.title_manual},
        {"indent", static_cast<std::int64_t>(item.indent)},
        {"selected", item.selected}, {"expanded", item.expanded}}));
  return values;
}

std::vector<std::size_t> NavigationModel::visible(std::string query) const {
  query = lower(std::move(query));
  std::vector<std::size_t> result;
  for (std::size_t index = 0; index < items_.size(); ++index) {
    bool shown = true;
    if (query.empty()) {
      auto required_indent = items_[index].indent - 1;
      for (auto previous = index; previous > 0 && required_indent >= 0;) {
        --previous;
        if (items_[previous].indent != required_indent)
          continue;
        if (!items_[previous].expanded)
          shown = false;
        --required_indent;
      }
    } else {
      shown = lower(items_[index].title).find(query) != std::string::npos;
      for (auto child = index + 1;
           !shown && child < items_.size() &&
           items_[child].indent > items_[index].indent; ++child)
        shown = lower(items_[child].title).find(query) != std::string::npos;
    }
    if (shown)
      result.push_back(index);
  }
  return result;
}

bool NavigationModel::select(const std::string_view id) {
  const auto found = std::ranges::find(items_, id, &DeskNavigationItem::id);
  if (found == items_.end())
    return false;
  for (auto& item : items_)
    item.selected = false;
  found->selected = true;
  if (found->kind != "session")
    found->expanded = !found->expanded;
  return true;
}

DeskNavigationItem* NavigationModel::selected() {
  const auto found = std::ranges::find(items_, true,
                                       &DeskNavigationItem::selected);
  return found == items_.end() ? nullptr : &*found;
}

const DeskNavigationItem* NavigationModel::selected() const {
  const auto found = std::ranges::find(items_, true,
                                       &DeskNavigationItem::selected);
  return found == items_.end() ? nullptr : &*found;
}

std::size_t NavigationModel::project_ancestor(const std::size_t index) const {
  if (index >= items_.size())
    return items_.size();
  if (items_[index].kind == "project")
    return index;
  auto indent = items_[index].indent;
  for (auto previous = index; previous > 0;) {
    --previous;
    if (items_[previous].indent >= indent)
      continue;
    indent = items_[previous].indent;
    if (items_[previous].kind == "project")
      return previous;
  }
  return items_.size();
}

std::filesystem::path NavigationModel::inherited_workspace(
    const std::size_t index) const {
  if (index < items_.size() && !items_[index].workspace.empty())
    return items_[index].workspace;
  const auto project = project_ancestor(index);
  if (project < items_.size() && !items_[project].workspace.empty())
    return items_[project].workspace;
  return default_workspace_;
}

DeskNavigationItem& NavigationModel::ensure_workspace_project(
    const std::filesystem::path& workspace) {
  const auto target = normalized(workspace, default_workspace_);
  for (std::size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].kind != "project" ||
        normalized(inherited_workspace(index), default_workspace_) != target)
      continue;
    for (auto& item : items_)
      item.selected = false;
    items_[index].selected = true;
    items_[index].expanded = true;
    return items_[index];
  }

  for (auto& item : items_)
    item.selected = false;
  auto title = target.filename().string();
  if (title.empty())
    title = target.root_name().string();
  items_.push_back({.id = tokmon::make_id("navigation"),
                    .workspace = target,
                    .kind = "project",
                    .title = title.empty() ? "工作区" : std::move(title),
                    .selected = true,
                    .expanded = true});
  return items_.back();
}

DeskNavigationItem& NavigationModel::create_session(std::string title) {
  std::size_t project = items_.size();
  if (const auto* active = selected())
    project = project_ancestor(static_cast<std::size_t>(active - items_.data()));
  if (project == items_.size()) {
    auto& ensured = ensure_workspace_project(default_workspace_);
    project = static_cast<std::size_t>(&ensured - items_.data());
  }
  for (auto& item : items_)
    item.selected = false;
  items_[project].expanded = true;
  auto insertion = project + 1;
  while (insertion < items_.size() &&
         items_[insertion].indent > items_[project].indent)
    ++insertion;
  auto created = DeskNavigationItem{
      .id = tokmon::make_id("session"), .workspace = {}, .kind = "session",
      .title = std::move(title), .indent = items_[project].indent + 1,
      .selected = true, .expanded = true, .title_manual = false};
  return *items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(insertion),
                        std::move(created));
}

bool NavigationModel::bind_selected_ray(std::string ray) {
  auto* item = selected();
  if (!item || item->kind != "session" || item->ray == ray)
    return false;
  item->ray = std::move(ray);
  return true;
}

bool NavigationModel::rename_selected(std::string title, const bool manual) {
  auto* item = selected();
  if (!item || item->kind != "session" || title.empty() || title.size() > 256)
    return false;
  const bool changed = item->title != title || item->title_manual != manual;
  item->title = std::move(title);
  item->title_manual = manual;
  return changed;
}

bool NavigationModel::remove_selected_session() {
  const auto found = std::ranges::find(items_, true,
                                       &DeskNavigationItem::selected);
  if (found == items_.end() || found->kind != "session")
    return false;
  const auto index = static_cast<std::size_t>(found - items_.begin());
  const auto project = project_ancestor(index);
  items_.erase(found);
  if (items_.empty())
    return true;
  for (auto& item : items_)
    item.selected = false;
  auto replacement = project < items_.size()
      ? project : std::min(index, items_.size() - 1);
  if (project < items_.size()) {
    for (auto child = project + 1;
         child < items_.size() &&
         items_[child].indent > items_[project].indent; ++child) {
      if (items_[child].kind == "session") {
        replacement = child;
        break;
      }
    }
  }
  items_[replacement].selected = true;
  return true;
}

std::filesystem::path NavigationModel::selected_workspace() const {
  const auto* item = selected();
  return item ? inherited_workspace(
                    static_cast<std::size_t>(item - items_.data()))
              : default_workspace_;
}

const std::vector<DeskNavigationItem>& NavigationModel::items() const noexcept {
  return items_;
}

std::vector<DeskNavigationItem>& NavigationModel::items() noexcept {
  return items_;
}

} // namespace tokmon::desk
