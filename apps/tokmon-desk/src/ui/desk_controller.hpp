#pragma once

#include <RmlUi/Core/EventListener.h>

#include "browser/browser_manager.hpp"
#include "editor/document_store.hpp"
#include "editor/syntax_service.hpp"
#include "integration/daemon_client.hpp"
#include "markdown/markdown_ast.hpp"
#include "review/git_service.hpp"
#include "terminal/terminal_service.hpp"
#include "ui/elements/element_code_surface.hpp"
#include "ui/elements/element_file_tree.hpp"
#include "ui/navigation_model.hpp"
#include "workspace/workspace_service.hpp"
#include "tokmon/daemon_lifecycle.hpp"

#include <filesystem>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Rml { class Element; class ElementDocument; }
union SDL_Event;

namespace tokmon::desk {

class SdlPlatform;

class DeskController final : public Rml::EventListener {
public:
  DeskController(Rml::ElementDocument& document, SdlPlatform& platform,
                 std::filesystem::path workspace, std::filesystem::path data_root,
                 std::filesystem::path daemon_endpoint = {});
  ~DeskController() override;

  void bind();
  void backend_connected();
  [[nodiscard]] bool update();
  [[nodiscard]] int update_poll_interval_ms() const noexcept;
  void ProcessEvent(Rml::Event& event) override;
  [[nodiscard]] bool quit_requested() const noexcept { return quit_requested_; }

private:
  void listen(const char* id, const char* event = "click");
  void show_toast(std::string message);
  void toggle_hidden(const char* id);
  void show_right_view(const char* id);
  void refresh_review();
  void toggle_branch_menu();
  void switch_branch(Rml::Element& element);
  void refresh_files();
  void load_tree_children(std::string relative_directory);
  void rebuild_file_tree();
  void handle_file_tree(float local_y);
  void search_files();
  void render_search_results(const std::vector<WorkspaceSearchResult>& results);
  void start_terminal();
  void resize_terminal_to_surface();
  void paste_terminal(bool allow_unsafe);
  void send_terminal_input();
  void search_terminal();
  void create_terminal_tab();
  void close_terminal_tab();
  void select_terminal_tab(std::string_view id);
  void render_terminal_tabs();
  void send_message();
  void choose_attachment();
  void apply_pending_photons();
  void render_conversation();
  void render_trajectory();
  void apply_settings(const tokmon::cbor::Value& payload);
  void render_navigation();
  void save_navigation();
  void handle_navigation(Rml::Element& item);
  void begin_workspace_switch(std::filesystem::path workspace,
                              bool create_session_after = false);
  void finish_workspace_switch();
  void filter_navigation();
  void request_selected_surface();
  void choose_starter(Rml::Element& card);
  void launch_browser();
  void refresh_browser();
  void back_browser();
  void forward_browser();
  void reload_browser();
  void toggle_browser_takeover();
  void stop_browser();
  void click_browser();
  void fill_browser();
  void render_browser_state(const BrowserSessionState& state);
  void create_session();
  void preview_file(Rml::Element& row);
  void save_file();
  void undo_file();
  void redo_file();
  void reload_file();
  void update_editor_status();
  void refresh_code_surface(bool preserve_caret);
  void apply_code_edit(const CodeEditIntent& intent);
  [[nodiscard]] bool handle_raw_event(const SDL_Event& event);
  void preview_diff(Rml::Element& row);
  void handle_git_action(Rml::Element& element);
  void confirm_discard();
  void commit_changes(bool push);
  void show_settings_page(Rml::Element& navigation);
  void render_settings_page(std::string page);
  void capture_settings_page();
  void save_settings();
  void reset_settings();
  void apply_providers(const tokmon::cbor::Value& payload);
  void enqueue_intent(std::string action, tokmon::cbor::Value payload);
  void start_next_intent();
  void finish_intent();
  static std::string escape(std::string_view text);

  struct TerminalTab {
    std::string id;
    std::string title;
    std::unique_ptr<TerminalSession> session;
    std::unique_ptr<GhosttyVt> vt;
    bool started{false};
    int columns{100};
    int rows{28};
  };
  [[nodiscard]] TerminalTab& active_terminal_tab();
  [[nodiscard]] TerminalSession& terminal_session();
  [[nodiscard]] GhosttyVt& terminal_vt();

  Rml::ElementDocument& document_;
  SdlPlatform& platform_;
  WorkspaceService workspace_;
  WorkspaceWatcher watcher_;
  GitService git_;
  BrowserManager browser_;
  DaemonClient daemon_;
  struct PendingIntent {
    std::string action;
    tokmon::cbor::Value payload;
  };
  std::deque<PendingIntent> intent_queue_;
  std::future<DaemonStreamResult> intent_future_;
  std::string intent_action_;
  std::future<DaemonStreamResult> chat_future_;
  std::future<DaemonStreamResult> startup_future_;
  std::string startup_action_;
  struct WorkspaceSwitchResult {
    std::filesystem::path workspace;
    std::filesystem::path endpoint;
    std::optional<tokmon::DaemonClientLease> lease;
    bool started{false};
    std::string error;
  };
  std::future<WorkspaceSwitchResult> workspace_switch_future_;
  std::optional<tokmon::DaemonClientLease> workspace_lease_;
  bool create_session_after_workspace_switch_{false};
  std::future<BrowserSessionState> browser_future_;
  std::string browser_session_{"tokmon-desk"};
  bool browser_takeover_{false};
  DocumentStore documents_;
  SyntaxService syntax_;
  MarkdownParser markdown_;
  NavigationModel navigation_;
  bool navigation_loaded_{false};
  tokmon::cbor::Value settings_values_{tokmon::cbor::Value::Map{}};
  tokmon::cbor::Value providers_payload_{tokmon::cbor::Value::Map{}};
  std::string settings_page_{"general"};
  std::string selected_provider_;
  std::string selected_model_;
  std::string selected_effort_{"高"};
  std::string selected_access_{"full"};
  bool pending_automatic_title_{false};
  std::future<std::vector<WorkspaceSearchResult>> file_search_future_;
  std::future<std::vector<WorkspaceEntry>> file_tree_future_;
  std::map<std::string, std::vector<WorkspaceEntry>> file_tree_children_;
  std::set<std::string> expanded_directories_;
  std::string loading_tree_directory_;
  std::string queued_tree_directory_;
  std::string pending_file_query_;
  std::filesystem::path current_file_;
  std::string current_diff_path_;
  bool current_diff_staged_{false};
  bool diff_split_view_{false};
  std::string pending_discard_path_;
  std::size_t pending_discard_hunk_{0};
  bool pending_discard_file_{false};
  std::uint64_t pending_discard_hash_{0};
  std::vector<std::unique_ptr<TerminalTab>> terminal_tabs_;
  std::size_t active_terminal_index_{0};
  std::uint64_t next_terminal_id_{1};
  std::mutex photon_mutex_;
  std::vector<tokmon::Photon> pending_photons_;
  std::vector<tokmon::Photon> photons_;
  std::uint64_t snow_cursor_{0};
  std::string active_ray_;
  bool startup_loaded_{false};
  std::atomic_bool backend_ready_{false};
  std::chrono::steady_clock::time_point startup_retry_at_{};
  bool conversation_dirty_{false};
  enum class HeavyFocus { none, editor, terminal } heavy_focus_{HeavyFocus::none};
  bool terminal_mouse_down_{false};
  std::string pending_terminal_keydown_text_;
  std::string pending_terminal_paste_;
  struct AttachmentDialogState {
    std::mutex mutex;
    std::filesystem::path selected;
    std::string error;
    std::atomic_bool complete{false};
  };
  std::shared_ptr<AttachmentDialogState> attachment_dialog_;
  std::filesystem::path selected_attachment_;
  std::chrono::steady_clock::time_point toast_until_{};
  bool quit_requested_{false};
};

} // namespace tokmon::desk
