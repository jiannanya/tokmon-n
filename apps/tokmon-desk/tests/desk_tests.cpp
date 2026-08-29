#include "app/app_state.hpp"
#include "browser/browser_manager.hpp"
#include "editor/document_store.hpp"
#include "editor/syntax_service.hpp"
#include "integration/daemon_client.hpp"
#include "lenses/common/process_runner.hpp"
#include "markdown/markdown_ast.hpp"
#include "platform/desk_app_paths.hpp"
#include "review/git_service.hpp"
#include "terminal/terminal_service.hpp"
#include "ui/navigation_model.hpp"
#include "workspace/workspace_service.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

bool run(const std::filesystem::path& cwd,
         std::vector<std::string> argv) {
  auto result = tokmon::builtin::run_process(
      std::move(argv), cwd, std::chrono::seconds(20), 1024u * 1024u);
  if (!result || result->exit_code != 0) {
    std::cerr << "fixture command failed";
    if (result)
      std::cerr << ": " << result->stderr_text;
    std::cerr << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  using namespace tokmon::desk;
  if (argc == 3 && std::string_view(argv[1]) == "--snow-endpoint") {
    DaemonClient live(argv[2]);
    const auto validate = live.stream_intent(
        "config.validate", tokmon::cbor::object({}), 0,
        [](tokmon::Photon) {});
    std::cout << "validate.success=" << validate.success
              << " payload=" << tokmon::cbor::diagnostic(validate.payload)
              << " error=" << validate.error << '\n';
    const auto settings = live.stream_intent(
        "settings.get", tokmon::cbor::object({}), validate.cursor,
        [](tokmon::Photon) {});
    std::cout << "settings.success=" << settings.success
              << " payload=" << tokmon::cbor::diagnostic(settings.payload)
              << " error=" << settings.error << '\n';
    return settings.success ? 0 : 1;
  }
  if ((argc == 3 || argc == 4) &&
      std::string_view(argv[1]) == "--browser-e2e") {
    BrowserManager browser(argv[2]);
    std::string browser_error;
    if (!browser.install_runtime(browser_error)) {
      std::cerr << browser_error << '\n';
      return 1;
    }
    const auto candidates = browser.discover();
    if (candidates.empty()) {
      std::cerr << "no system Chrome/Chromium found\n";
      return 1;
    }
    const std::string session = "tokmon-desk-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::cout << "browser.executable=" << candidates.front().executable
              << " session=" << session << '\n';
    const std::string test_url = argc == 4 ? argv[3] : "https://example.com";
    auto state = browser.open(candidates.front().executable, session,
                              test_url, false);
    std::cout << "browser.running=" << state.running
              << " title=" << state.title
              << " url=" << state.url
              << " snapshot=" << state.accessibility_snapshot << '\n';
    bool valid = state.running &&
                  std::filesystem::is_regular_file(state.preview_image);
    auto previous_preview = state.preview_image;
    if (argc == 3) {
      valid = valid && state.title == "Example Domain" &&
              state.url == "https://example.com/" &&
              state.accessibility_snapshot.find("ref=e2") !=
                  std::string::npos;
    } else {
      valid = valid && state.title == "Tokmon Browser E2E" &&
              state.accessibility_snapshot.find("E2E name") !=
                  std::string::npos;
      valid = browser.fill(session, "#e2e-name", "Tokmon", browser_error) &&
              valid;
      valid = browser.click(session, "#e2e-submit", browser_error) && valid;
      state = browser.refresh(session);
      valid = state.running &&
              state.preview_image != previous_preview &&
              std::filesystem::is_regular_file(state.preview_image) &&
              state.accessibility_snapshot.find("Hello Tokmon") !=
                  std::string::npos && valid;
      previous_preview = state.preview_image;
      valid = browser.click(session, "#e2e-next", browser_error) && valid;
      state = browser.refresh(session);
      valid = state.running && state.preview_image != previous_preview &&
              state.title == "Tokmon Browser E2E Next" && valid;
      state = browser.back(session);
      valid = state.running && state.title == "Tokmon Browser E2E" && valid;
      state = browser.forward(session);
      valid = state.running && state.title == "Tokmon Browser E2E Next" && valid;
      state = browser.reload(session);
      valid = state.running && state.title == "Tokmon Browser E2E Next" && valid;
    }
    if (!browser.close(session, browser_error)) {
      std::cerr << browser_error << '\n';
      return 1;
    }
    return valid ? 0 : 1;
  }
  const auto root = std::filesystem::temp_directory_path() /
      ("tokmon-desk-tests-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "src");
  const auto file = root / "src" / "sample.md";
  {
    std::ofstream output(file, std::ios::binary);
    output << "# Hello\n\n**world** <script>alert(1)</script>\n";
  }
  const auto cpp_file = root / "src" / "sample.cpp";
  {
    std::ofstream output(cpp_file, std::ios::binary);
    for (int line = 1; line <= 14; ++line)
      output << "int value_" << line << " = " << line << ";\n";
  }

  check(run(root, {"git", "init", "-b", "main"}), "Git fixture initializes");
  check(run(root, {"git", "config", "user.name", "Tokmon Desk Tests"}),
        "Git fixture name configured");
  check(run(root, {"git", "config", "user.email", "desk-tests@localhost"}),
        "Git fixture email configured");
  check(run(root, {"git", "add", "--all"}), "Git fixture stages baseline");
  check(run(root, {"git", "commit", "-m", "baseline"}),
        "Git fixture commits baseline");

  WorkspaceService workspace(root);
  check(workspace.root() == std::filesystem::weakly_canonical(root),
        "workspace root canonicalization");
  check(workspace.contains(file), "workspace contains child file");
  check(!workspace.contains(root.parent_path() / "outside.txt"),
        "workspace rejects outside file");
  std::string error;
  check(workspace.read_text(file, 4096, error).find("Hello") != std::string::npos,
        "workspace reads UTF-8 text");
  const auto search_results = workspace.search("alert(1)", 10);
  check(search_results.size() == 1 && search_results.front().line == 3,
        "workspace content search returns line and preview");
  const auto async_entries = workspace.enumerate_async(50, 4).get();
  check(async_entries.size() >= 2, "workspace enumeration runs asynchronously");

  WorkspaceWatcher watcher(root);
  const auto watched_file = root / "src" / "watch-me.txt";
  {
    std::ofstream output(watched_file, std::ios::binary);
    output << "watcher event\n";
  }
  bool watcher_created = false;
  for (int attempt = 0; attempt < 30 && !watcher_created; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    for (const auto& change : watcher.take_changes())
      watcher_created = watcher_created ||
          (change.path.filename() == watched_file.filename() &&
           change.kind == WorkspaceChangeKind::created);
  }
  check(watcher_created, "workspace watcher reports created file");

  DocumentStore documents;
  auto opened = documents.open(file, error);
  check(opened.has_value() && opened->version == 1, "document opens at version one");
  check(opened && documents.edit(file, opened->text.size(), 0, "edited\n",
                                 opened->version, error),
        "document versioned edit");
  auto edited = documents.snapshot(file);
  check(edited && edited->dirty && edited->version == 2, "document dirty snapshot");
  check(edited && documents.undo(file, edited->version, error),
        "document undo applies inverse edit");
  auto undone = documents.snapshot(file);
  check(undone && !undone->dirty && undone->can_redo,
        "document undo restores clean baseline");
  check(undone && documents.redo(file, undone->version, error),
        "document redo reapplies edit");
  edited = documents.snapshot(file);
  check(edited && edited->dirty && edited->can_undo,
        "document redo restores dirty state");
  check(edited && documents.save(file, edited->version, error), "document atomic save");

  auto conflict = documents.snapshot(file);
  check(conflict && documents.edit(file, 0, 0, "local\n", conflict->version, error),
        "document accepts local edit before conflict");
  {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << "external\n";
  }
  documents.observe_external_change(file);
  conflict = documents.snapshot(file);
  check(conflict && conflict->dirty && conflict->external_conflict,
        "dirty document detects external conflict");
  check(conflict && !documents.save(file, conflict->version, error),
        "conflicted document refuses silent overwrite");
  check(documents.reload(file, true, error),
        "explicit reload resolves external conflict");

  const auto large_file = root / "src" / "large.cpp";
  {
    std::ofstream output(large_file, std::ios::binary);
    for (int line = 1; line <= 100000; ++line)
      output << "int line_" << line << " = " << line << ";\n";
  }
  const auto large_open_started = std::chrono::steady_clock::now();
  auto large = documents.open(large_file, error);
  const auto large_open_elapsed = std::chrono::steady_clock::now() -
                                  large_open_started;
  check(large && std::count(large->text.begin(), large->text.end(), '\n') ==
                     100000,
        "100k-line document opens without truncation");
  check(large_open_elapsed < std::chrono::seconds(5),
        "100k-line document opens within hard-gate budget");
  check(large && documents.edit(large_file, large->text.size(), 0,
                                 "// tail edit\n", large->version, error),
        "100k-line document accepts tail edit");
  auto large_edited = documents.snapshot(large_file);
  check(large_edited &&
            documents.undo(large_file, large_edited->version, error),
        "100k-line document undo remains functional");

  const auto binary_file = root / "src" / "binary.bin";
  {
    std::ofstream output(binary_file, std::ios::binary);
    const char bytes[] = {'a', '\0', 'b'};
    output.write(bytes, sizeof(bytes));
  }
  check(!documents.open(binary_file, error),
        "binary document is rejected from text editor");
  const auto crlf_file = root / "src" / "crlf.txt";
  {
    std::ofstream output(crlf_file, std::ios::binary);
    output << "\xef\xbb\xbfline one\r\nline two\r\n";
  }
  auto crlf = documents.open(crlf_file, error);
  check(crlf && crlf->text.starts_with("\xef\xbb\xbf") &&
            crlf->text.find("\r\n") != std::string::npos,
        "UTF-8 BOM and CRLF are preserved");

  SyntaxService syntax;
  std::string syntax_error;
  check(syntax.update_cpp("int main() { return 0; }\n", syntax_error),
        "tree-sitter parses C++ document");
  const auto initial_spans = syntax.spans();
  check(!initial_spans.empty(), "tree-sitter returns syntax spans");
  check(syntax.update_cpp("// edited\nint main() { return 1; }\n", syntax_error),
        "tree-sitter incrementally reparses edit");
  check(!syntax.spans(0, 10).empty(),
        "tree-sitter clips spans to visible byte range");

  MarkdownParser parser;
  const auto markdown = parser.parse("# Title\n\n[link](javascript:bad) **ok** <b>x</b>");
  const auto rml = markdown_to_safe_rml(markdown);
  check(!markdown.nodes.empty(), "markdown produces owned AST");
  check(rml.find("javascript:") == std::string::npos, "markdown removes unsafe link");
  check(rml.find("&lt;b&gt;") != std::string::npos, "markdown escapes raw HTML");

  GhosttyVt vt(40, 5);
  vt.append("red\x1b[31m text\x1b[0m\n");
  check(vt.plain_text().find("red text") != std::string::npos,
        "libghostty-vt consumes ANSI control sequences");
  const auto terminal_snapshot = vt.render_snapshot();
  check(terminal_snapshot.columns == 40 && terminal_snapshot.rows == 5 &&
            terminal_snapshot.cells.size() == 200,
        "libghostty-vt exposes complete render grid");
  check(std::ranges::any_of(terminal_snapshot.cells, [](const auto& cell) {
          return !cell.grapheme.empty();
        }),
        "libghostty-vt render grid contains terminal graphemes");
  check(terminal_snapshot.default_foreground.red !=
                terminal_snapshot.default_background.red ||
            terminal_snapshot.default_foreground.green !=
                terminal_snapshot.default_background.green ||
            terminal_snapshot.default_foreground.blue !=
                terminal_snapshot.default_background.blue,
        "libghostty-vt terminal theme has readable foreground contrast");
  check(vt.encode_key(TerminalKey::unidentified, "x", 0) == "x",
        "libghostty-vt encodes raw UTF-8 terminal text input");
  check(vt.encode_key(TerminalKey::enter, {}, 0) == "\r",
        "libghostty-vt encodes terminal Enter key");
  std::string encoded_paste;
  vt.set_response_sink([&encoded_paste](const std::string_view bytes) {
    encoded_paste.append(bytes);
  });
  vt.append("\x1b[?2004h");
  check(vt.paste("one\ntwo") == TerminalPasteResult::written &&
            encoded_paste == "\x1b[200~one\ntwo\x1b[201~",
        "libghostty-vt applies bracketed paste mode");
  encoded_paste.clear();
  vt.append("\x1b[?2004l");
  check(vt.paste("echo one\necho two") == TerminalPasteResult::unsafe &&
            encoded_paste.empty(),
        "libghostty-vt rejects unsafe multiline paste before confirmation");
  check(vt.paste("echo one\necho two", true) ==
            TerminalPasteResult::written && !encoded_paste.empty(),
        "confirmed terminal paste is encoded and delivered");
  GhosttyVt link_vt(40, 4);
  link_vt.append("\x1b]8;;https://example.com\x1b\\Example"
                 "\x1b]8;;\x1b\\\r\n");
  const auto link_snapshot = link_vt.render_snapshot();
  check(std::ranges::any_of(link_snapshot.cells, [](const auto& cell) {
          return cell.hyperlink == "https://example.com";
        }),
        "libghostty-vt exposes OSC 8 URI on rendered cells");
  check(link_vt.resize(120, 40, 8, 16),
        "libghostty-vt resizes and reflows terminal grid");

  TerminalSession terminal;
  std::string terminal_error;
  const bool terminal_started = terminal.start({}, root, 80, 24, terminal_error);
  check(terminal_started, "platform PTY starts");
  bool terminal_echo = false;
  if (terminal_started) {
    check(terminal.write("echo TOKMON_TERMINAL_OK\r\n", terminal_error),
          "platform PTY accepts input");
    std::string terminal_output;
    for (int attempt = 0; attempt < 40 && !terminal_echo; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      terminal_output += terminal.take_output();
      terminal_echo = terminal_output.find("TOKMON_TERMINAL_OK") != std::string::npos;
    }
    terminal.stop();
    check(terminal_echo, "platform PTY returns shell output");
  }

  const auto state = AppState::make_initial(root);
  check(state.navigation.size() == 1 && state.messages.empty(),
        "initial UI state contains no fabricated session data");

  NavigationModel navigation(root);
  const auto navigation_value = tokmon::cbor::Value::Array{
      tokmon::cbor::object({{"id", "g"}, {"kind", "group"},
                            {"title", "默认"}, {"indent", 0},
                            {"expanded", true}}),
      tokmon::cbor::object({{"id", "p"}, {"kind", "project"},
                            {"title", "fixture"}, {"indent", 1},
                            {"workspace", root.generic_string()},
                            {"selected", true}})};
  check(navigation.load(navigation_value, error),
        "navigation model validates persisted hierarchy");
  auto& created_session = navigation.create_session("新会话");
  check(created_session.kind == "session" && created_session.indent == 2 &&
            navigation.selected() == &created_session,
        "navigation model inserts session under selected project");
  check(navigation.bind_selected_ray("ray-test") &&
            navigation.encode().as_array()->size() == 3,
        "navigation model binds Ray and serializes complete hierarchy");
  const auto second_workspace = root / "second-workspace";
  std::filesystem::create_directories(second_workspace);
  auto& second_project = navigation.ensure_workspace_project(second_workspace);
  check(second_project.kind == "project" &&
            second_project.workspace ==
                std::filesystem::weakly_canonical(second_workspace) &&
            navigation.selected() == &second_project,
        "navigation model creates and selects the current workspace project");
  const auto second_project_indent = second_project.indent;
  auto& second_session = navigation.create_session("第二工作区会话");
  check(navigation.selected_workspace() ==
            std::filesystem::weakly_canonical(second_workspace) &&
            second_session.indent == second_project_indent + 1,
        "navigation session stays under its matching workspace project");

  const auto app_paths = DeskAppPaths::resolve();
  check(app_paths.isolated_from(root),
        "desktop data roots are isolated from workspace and .tokmon");

#ifdef TOKMON_DESK_SOURCE_DIR
  GitService git(root);
  const auto git_status = git.status();
  check(git_status.repository && !git_status.branch.empty(),
        "libgit2 discovers repository and branch");
  {
    std::ofstream output(cpp_file, std::ios::binary | std::ios::trunc);
    for (int line = 1; line <= 14; ++line) {
      if (line == 2)
        output << "int value_2 = 200;\n";
      else if (line == 13)
        output << "int value_13 = 1300;\n";
      else
        output << "int value_" << line << " = " << line << ";\n";
    }
  }
  auto worktree_diff = git.diff_model("src/sample.cpp", false, error);
  check(worktree_diff && worktree_diff->hunks.size() == 2,
        "libgit2 produces structured multi-hunk diff");
  check(git.stage_hunk("src/sample.cpp", 0, error),
        "libgit2 stages one hunk");
  auto staged_diff = git.diff_model("src/sample.cpp", true, error);
  check(staged_diff && staged_diff->hunks.size() == 1,
        "staging one hunk leaves another unstaged");
  check(git.unstage_hunk("src/sample.cpp", 0, error),
        "libgit2 unstages one hunk");
  staged_diff = git.diff_model("src/sample.cpp", true, error);
  check(staged_diff && staged_diff->hunks.empty(),
        "unstaging hunk restores clean index");
  worktree_diff = git.diff_model("src/sample.cpp", false, error);
  const auto cpp_text = workspace.read_text(cpp_file, 1024u * 1024u, error);
  check(worktree_diff && worktree_diff->hunks.size() == 2 &&
            git.discard_hunk("src/sample.cpp", 1,
                             DocumentStore::content_hash(cpp_text), error),
        "guarded discard reverts selected hunk");
  check(git.stage_file("src/sample.cpp", error), "libgit2 stages file");
  check(git.unstage_file("src/sample.cpp", error), "libgit2 unstages file");
  const auto before_discard = workspace.read_text(cpp_file, 1024u * 1024u, error);
  check(git.discard_file("src/sample.cpp",
                         DocumentStore::content_hash(before_discard), false, error),
        "guarded discard restores tracked file");
  check(run(root, {"git", "add", "--all"}) &&
            run(root, {"git", "commit", "-m", "branch fixture"}),
        "Git fixture is clean before safe branch checkout");
  check(run(root, {"git", "branch", "desk-branch"}),
        "Git fixture creates a branch for checkout coverage");
  const auto branches = git.branches(error);
  check(std::ranges::find(branches, "desk-branch") != branches.end(),
        "libgit2 enumerates local branches");
  check(git.checkout_branch("desk-branch", error) &&
            git.status().branch == "desk-branch",
        "libgit2 safely switches a clean worktree branch");
  check(git.checkout_branch("main", error) && git.status().branch == "main",
        "libgit2 switches back to the original branch");
  check(git.diff_model("../outside.txt", false, error) == std::nullopt,
        "Git operations reject workspace path escape");
#endif

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  if (failures == 0)
    std::cout << "tokmon-desk core tests passed\n";
  return failures == 0 ? 0 : 1;
}
