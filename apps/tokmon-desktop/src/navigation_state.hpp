#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

namespace tokmon::desktop {

void refresh_navigation(
    const std::shared_ptr<slint::VectorModel<NavigationItem>> &model,
    const std::shared_ptr<std::vector<NavigationItem>> &items,
    std::string query,
    const slint::ComponentWeakHandle<MainWindow> &window = {});

std::string short_workspace_label(const std::filesystem::path &workspace,
                                  const std::filesystem::path &root_workspace);
std::string git_branch_label(const std::filesystem::path &workspace);
int count_indexed_files(const std::filesystem::path &workspace);

NavigationItem make_navigation_item(const std::filesystem::path &assets,
                                    std::string id, std::string kind,
                                    std::string title, int indent,
                                    bool selected, bool expanded = true,
                                    std::string ray = {},
                                    std::string workspace = {});
tokmon::cbor::Value navigation_value(const std::vector<NavigationItem> &items);
std::optional<std::vector<NavigationItem>>
navigation_items(const tokmon::cbor::Value &value,
                 const std::filesystem::path &assets,
                 const std::filesystem::path &default_workspace);
std::filesystem::path
navigation_workspace_at(const std::vector<NavigationItem> &items,
                        std::size_t index,
                        const std::filesystem::path &fallback);
std::size_t navigation_ancestor_at(const std::vector<NavigationItem> &items,
                                   std::size_t index, std::string_view kind);

} // namespace tokmon::desktop
