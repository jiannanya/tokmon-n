#include "app/app_state.hpp"
#include "browser/browser_manager.hpp"
#include "editor/document_store.hpp"
#include "editor/grapheme.hpp"
#include "editor/syntax_service.hpp"
#include "fonts/font_manager.hpp"
#include "integration/daemon_client.hpp"
#include "lenses/common/process_runner.hpp"
#include "markdown/markdown_ast.hpp"
#include "platform/desk_app_paths.hpp"
#include "review/git_service.hpp"
#include "review/desktop_change_tracker.hpp"
#include "state/desk_state_store.hpp"
#include "state/document_recovery.hpp"
#include "terminal/terminal_service.hpp"
#include "ui/navigation_model.hpp"
#include "ui/theme_palette.hpp"
#include "workspace/workspace_service.hpp"

#include "tokmon/hash.hpp"
#include "tokmon/json.hpp"
#include "tokmon/snow_transport.hpp"

#include <chrono>
#include <atomic>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
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

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::uintmax_t directory_bytes(const std::filesystem::path& root) {
  std::uintmax_t bytes = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error))
      bytes += iterator->file_size(error);
    error.clear();
  }
  return bytes;
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

const tokmon::cbor::Value* field(const tokmon::cbor::Value& value,
                                std::string_view name) {
  return tokmon::cbor::find(value, name);
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

  {
    const auto endpoint = tokmon::default_snow_endpoint(root / "cancel-snow");
    std::atomic<std::uint64_t> active_request{0};
    std::atomic<std::uint64_t> cancelled_request{0};
    tokmon::SnowServer server;
    check(static_cast<bool>(server.start(endpoint,
        [&active_request, &cancelled_request](
            const tokmon::SnowMessage& request) {
          if (request.kind == tokmon::SnowMessageKind::cancel) {
            const auto* target = tokmon::cbor::find(request.payload,
                                                    "request_id");
            if (target)
              cancelled_request.store(
                  static_cast<std::uint64_t>(target->as_integer()),
                  std::memory_order_release);
            return tokmon::SnowMessage{
                .kind = tokmon::SnowMessageKind::intent_result,
                .request_id = request.request_id,
                .payload = tokmon::cbor::object({{"cancel_requested", true}})};
          }
          active_request.store(request.request_id, std::memory_order_release);
          const auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
          while (cancelled_request.load(std::memory_order_acquire) !=
                     request.request_id &&
                 std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
          return tokmon::SnowMessage{
              .kind = tokmon::SnowMessageKind::intent_result,
              .request_id = request.request_id,
              .payload = tokmon::cbor::object({
                  {"cancelled", cancelled_request.load(
                      std::memory_order_acquire) == request.request_id}})};
        })), "desk cancellation fixture starts");
    DaemonClient client(endpoint);
    const auto request_id = tokmon::next_snow_request_id();
    auto running = std::async(std::launch::async, [&client, request_id] {
      return client.stream_intent("chat", tokmon::cbor::object({}), 0,
                                  [](tokmon::Photon) {}, request_id);
    });
    const auto active_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(2);
    while (active_request.load(std::memory_order_acquire) != request_id &&
           std::chrono::steady_clock::now() < active_deadline)
      std::this_thread::yield();
    std::string cancel_error;
    check(client.cancel(request_id, cancel_error),
          "desk sends a real Snow cancel for the active request id");
    const auto cancelled = running.get();
    check(cancelled.success &&
              field(cancelled.payload, "cancelled") &&
              field(cancelled.payload, "cancelled")->as_bool() &&
              cancelled_request.load(std::memory_order_acquire) == request_id,
          "desk cancellation targets and releases the running stream");
    server.stop();
  }

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
  write_file(root / ".gitignore",
             "ignored/\n*.cache\nnested/*.secret\n"
             "!nested/keep.secret\ndirectory-rule/\n");
  write_file(root / "ignored" / "secret.txt", "must stay hidden\n");
  write_file(root / "src" / "scratch.cache", "must stay hidden\n");
  write_file(root / "nested" / "drop.secret", "must stay hidden\n");
  write_file(root / "nested" / "keep.secret", "negation keeps this visible\n");
  write_file(root / "directory-rule" / "inside.txt", "must stay hidden\n");
  write_file(root / "src" / "duplicate-a.txt", "deduplicated preimage\n");
  write_file(root / "src" / "duplicate-b.txt", "deduplicated preimage\n");

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
  const auto ignored_hidden = std::ranges::none_of(async_entries, [](const auto& entry) {
          return entry.relative_path.starts_with("ignored") ||
                 entry.relative_path.ends_with(".cache") ||
                 entry.relative_path == "nested/drop.secret" ||
                 entry.relative_path.starts_with("directory-rule");
        });
  const auto negated_visible = std::ranges::any_of(async_entries, [](const auto& entry) {
    return entry.relative_path == "nested/keep.secret";
  });
  if (!ignored_hidden) {
    for (const auto& entry : async_entries)
      if (entry.relative_path.starts_with("ignored") ||
          entry.relative_path.ends_with(".cache"))
        std::cerr << "Ignore detail: " << entry.relative_path << '\n';
  }
  check(ignored_hidden && negated_visible,
        "workspace tree applies nested, negated, wildcard, and directory .gitignore rules");
  std::atomic_bool cancelled_search{true};
  check(workspace.search("Hello", 100, &cancelled_search).empty(),
        "workspace search observes cancellation without publishing stale results");
  const auto outside_directory = std::filesystem::path(root.string() + "-outside");
  std::filesystem::create_directories(outside_directory);
  write_file(outside_directory / "outside.txt", "outside\n");
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(outside_directory,
                                             root / "escape-link",
                                             symlink_error);
  if (!symlink_error)
    check(!workspace.contains(root / "escape-link" / "outside.txt") &&
              !workspace.create_file("escape-link/created.txt", "bad", error),
          "workspace rejects symlink/junction escape from the canonical root");
  check(workspace.create_directory("generated", error),
        "workspace creates a contained directory");
  check(workspace.create_file("generated/new.txt", "new\n", error),
        "workspace creates a contained file");
  check(!workspace.create_file("generated/new.txt", "collision\n", error),
        "workspace create reports an existing-path conflict");
  check(!workspace.create_file(root.parent_path() / "absolute-escape.txt",
                               "bad", error),
        "workspace mutation rejects absolute path escape");
  check(workspace.rename_entry("generated/new.txt", "renamed.txt", error) &&
            std::filesystem::is_regular_file(root / "generated/renamed.txt"),
        "workspace safely renames an entry");
  check(!workspace.create_file("../escape.txt", "bad", error),
        "workspace mutation rejects parent traversal");
  check(!workspace.remove_entry(".git", true, error),
        "workspace mutation protects Git metadata");
  check(workspace.remove_entry("generated", true, error) &&
            !std::filesystem::exists(root / "generated"),
        "workspace recursively removes an explicitly selected directory");

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
  (void)watcher.take_changes();
  watcher.acknowledge_self_write(watched_file);
  write_file(watched_file, "self save\n");
  bool watcher_self = false;
  for (int attempt = 0; attempt < 30 && !watcher_self; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& change : watcher.take_changes())
      watcher_self = watcher_self ||
          (change.path.filename() == watched_file.filename() &&
           change.origin == WorkspaceChangeOrigin::self);
  }
  check(watcher_self, "workspace watcher distinguishes a Desktop self-save");
  write_file(watched_file, "external edit\n");
  bool watcher_external = false;
  for (int attempt = 0; attempt < 30 && !watcher_external; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& change : watcher.take_changes())
      watcher_external = watcher_external ||
          (change.path.filename() == watched_file.filename() &&
           change.origin == WorkspaceChangeOrigin::external);
  }
  check(watcher_external, "workspace watcher classifies later external edits");
  const auto renamed_watch_file = root / "src" / "watch-renamed.txt";
  std::filesystem::rename(watched_file, renamed_watch_file);
  for (int index = 0; index < 8; ++index)
    write_file(root / "src" / ("watch-burst-" + std::to_string(index) + ".txt"),
               "burst\n");
  std::filesystem::remove(renamed_watch_file);
  bool watcher_rename_old = false;
  bool watcher_rename_new = false;
  bool watcher_delete = false;
  std::size_t watcher_burst_files = 0;
  for (int attempt = 0; attempt < 60 &&
       (!watcher_rename_old || !watcher_rename_new || !watcher_delete ||
        watcher_burst_files < 8); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (const auto& change : watcher.take_changes()) {
      const auto name = change.path.filename().string();
      watcher_rename_old = watcher_rename_old ||
          (name == "watch-me.txt" && change.kind == WorkspaceChangeKind::removed);
      watcher_rename_new = watcher_rename_new ||
          (name == "watch-renamed.txt" && change.kind == WorkspaceChangeKind::created);
      watcher_delete = watcher_delete ||
          (name == "watch-renamed.txt" && change.kind == WorkspaceChangeKind::removed);
      if (name.starts_with("watch-burst-") &&
          change.kind == WorkspaceChangeKind::created)
        ++watcher_burst_files;
    }
  }
  check(watcher_rename_old && watcher_rename_new && watcher_delete &&
            watcher_burst_files >= 8,
        "workspace watcher coalesces burst and reports rename/delete semantics");

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
  check(std::ranges::none_of(std::filesystem::directory_iterator(file.parent_path()),
                            [](const auto& entry) {
                              return entry.path().filename().string().find(
                                         ".tokmon-desk.tmp-") !=
                                     std::string::npos;
                            }),
        "durable document save leaves no same-directory temporary artifact");

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
  check(crlf && crlf->encoding == TextEncoding::utf8_bom &&
            crlf->line_ending == LineEnding::crlf,
        "document reports BOM encoding and CRLF metadata");
  check(crlf && documents.edit(crlf_file, crlf->text.size(), 0,
                               "tail\r\n", crlf->version, error),
        "BOM/CRLF document accepts a preserving edit");
  crlf = documents.snapshot(crlf_file);
  check(crlf && documents.save(crlf_file, crlf->version, error) &&
            read_file(crlf_file).starts_with("\xef\xbb\xbf") &&
            read_file(crlf_file).find("tail\r\n") != std::string::npos,
        "durable atomic save preserves UTF-8 BOM and CRLF bytes");
  const auto readonly_file = root / "src" / "readonly.txt";
  write_file(readonly_file, "read only\n");
  const auto original_permissions =
      std::filesystem::status(readonly_file).permissions();
  std::filesystem::permissions(
      readonly_file,
      std::filesystem::perms::owner_write |
          std::filesystem::perms::group_write |
          std::filesystem::perms::others_write,
      std::filesystem::perm_options::remove);
  DocumentStore readonly_documents;
  const auto readonly = readonly_documents.open(readonly_file, error);
  check(readonly && readonly->read_only &&
            !readonly_documents.edit(readonly_file, 0, 0, "bad",
                                     readonly->version, error),
        "document detects read-only files and rejects mutation");
  std::filesystem::permissions(readonly_file, original_permissions,
                               std::filesystem::perm_options::replace);

  const auto random_file = root / "src" / "random-edits.txt";
  write_file(random_file, "seed\n");
  DocumentStore random_documents;
  auto random_snapshot = random_documents.open(random_file, error);
  std::string random_expected = "seed\n";
  constexpr int random_edit_count = 128;
  bool random_edits_ok = random_snapshot.has_value();
  for (int index = 0; index < random_edit_count && random_edits_ok; ++index) {
    const auto insertion = "edit-" + std::to_string(index) + "\n";
    random_edits_ok = random_documents.edit(
        random_file, random_snapshot->text.size(), 0, insertion,
        random_snapshot->version, error);
    random_expected += insertion;
    random_snapshot = random_documents.snapshot(random_file);
    random_edits_ok = random_edits_ok && random_snapshot.has_value();
  }
  for (int index = 0; index < random_edit_count && random_edits_ok; ++index) {
    random_edits_ok = random_documents.undo(random_file,
                                             random_snapshot->version, error);
    random_snapshot = random_documents.snapshot(random_file);
    random_edits_ok = random_edits_ok && random_snapshot.has_value();
  }
  const bool random_undo_exact = random_edits_ok &&
      random_snapshot->text == "seed\n" && !random_snapshot->dirty;
  for (int index = 0; index < random_edit_count && random_edits_ok; ++index) {
    random_edits_ok = random_documents.redo(random_file,
                                             random_snapshot->version, error);
    random_snapshot = random_documents.snapshot(random_file);
    random_edits_ok = random_edits_ok && random_snapshot.has_value();
  }
  check(random_undo_exact && random_edits_ok &&
            random_snapshot->text == random_expected && random_snapshot->dirty,
        "document random edit sequence has exact undo/redo and save-point state");
  const auto large_metadata_file = root / "src" / "large-metadata.txt";
  write_file(large_metadata_file, std::string(9u * 1024u * 1024u, 'x'));
  const auto large_metadata = documents.open(large_metadata_file, error);
  check(large_metadata && large_metadata->large_file,
        "document flags files above 8 MiB for reduced-cost UI behavior");
  const auto recovery_file = root / "src" / "recover-me.txt";
  write_file(recovery_file, "disk baseline\n");
  auto recovery_document = documents.open(recovery_file, error);
  check(recovery_document && documents.edit(
            recovery_file, recovery_document->text.size(), 0, "unsaved\n",
            recovery_document->version, error),
        "document prepares unsaved recovery fixture");
  recovery_document = documents.snapshot(recovery_file);
  const auto recovery_root =
      std::filesystem::path(root.string() + "-document-recovery");
  DocumentRecoveryStore recovery_store(recovery_root);
  check(recovery_document && recovery_store.save(*recovery_document, root, error),
        "document recovery atomically saves dirty Desktop-private state");
  const auto recovered = recovery_store.load(recovery_file, root, error);
  check(recovered && recovered->text == recovery_document->text &&
            recovered->disk_hash == recovery_document->disk_hash,
        "document recovery round-trips exact unsaved bytes and disk guard");
  check(!recovery_store.load(root.parent_path() / "outside.txt", root, error),
        "document recovery rejects paths outside the workspace");
  check(recovery_store.clear(recovery_file, error) &&
            !recovery_store.load(recovery_file, root, error),
        "document recovery is removed after a successful save");

  const std::string combining = "e\xcc\x81";
  const std::string family =
      "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
      "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6";
  const std::string flag = "\xf0\x9f\x87\xa8\xf0\x9f\x87\xb3";
  const std::string toned = "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd";
  check(grapheme_count(combining) == 1 &&
            next_grapheme_boundary(combining, 0) == combining.size(),
        "grapheme navigation keeps combining marks attached");
  check(grapheme_count(family) == 1 && grapheme_count(flag) == 1 &&
            grapheme_count(toned) == 1,
        "grapheme navigation keeps ZWJ emoji, flags, and skin tones atomic");
  check(grapheme_count("\r\n") == 1 &&
            previous_grapheme_boundary(family, family.size()) == 0,
        "grapheme navigation handles CRLF and reverse movement");

#ifdef TOKMON_DESK_FONT_FILE
  FontManager font_manager;
  std::string font_error;
  const bool font_loaded = font_manager.load_ui_font(TOKMON_DESK_FONT_FILE,
                                                      font_error);
  const std::array<std::string, 5> shaping_samples{
      "Tokmon English 中文", "مرحبا بالعالم", "e\xcc\x81",
      family, flag + toned};
  bool shaping_valid = font_loaded && font_manager.ready();
  for (const auto& sample : shaping_samples) {
    const auto shaped = font_manager.shape_utf8(sample, 13.f);
    shaping_valid = shaping_valid && !shaped.empty() &&
        std::ranges::all_of(shaped, [](const auto& glyph) {
          return std::isfinite(glyph.x_advance) &&
                 std::isfinite(glyph.y_advance) &&
                 std::isfinite(glyph.x_offset) &&
                 std::isfinite(glyph.y_offset);
        });
  }
  const auto shaped_first = font_manager.shape_utf8("缓存与 DPI", 13.f);
  const auto shaped_repeat = font_manager.shape_utf8("缓存与 DPI", 13.f);
  const auto shaped_larger = font_manager.shape_utf8("缓存与 DPI", 26.f);
  const auto advance = [](const auto& glyphs) {
    float result = 0.f;
    for (const auto& glyph : glyphs)
      result += glyph.x_advance;
    return result;
  };
  check(shaping_valid && shaped_first.size() == shaped_repeat.size() &&
            std::abs(advance(shaped_first) - advance(shaped_repeat)) < 0.01f &&
            advance(shaped_larger) > advance(shaped_first),
        "HarfBuzz/FreeType shapes Latin, CJK, Arabic, combining and emoji runs deterministically across sizes");
#endif

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
  const std::vector<std::pair<std::string, SyntaxLanguage>> language_paths = {
      {"main.rs", SyntaxLanguage::rust}, {"app.tsx", SyntaxLanguage::tsx},
      {"run.py", SyntaxLanguage::python}, {"data.json", SyntaxLanguage::json},
      {"ci.yml", SyntaxLanguage::yaml}, {"Cargo.toml", SyntaxLanguage::toml},
      {"README.md", SyntaxLanguage::markdown}, {"build.ps1", SyntaxLanguage::shell},
      {"CMakeLists.txt", SyntaxLanguage::cmake}};
  for (const auto& [path, expected] : language_paths)
    check(syntax_language_for_path(path) == expected,
          "editor detects a supported language from its path");
  check(syntax.update(SyntaxLanguage::python,
                      "# note\ndef answer():\n    return 42\n", syntax_error) &&
            !syntax.spans().empty() && syntax.language() == SyntaxLanguage::python,
        "editor syntax adapter supports non-C++ language documents");
  const std::vector<std::pair<SyntaxLanguage, std::string>> grammar_fixtures = {
      {SyntaxLanguage::rust, "// note\nfn main() { let value = 42; }\n"},
      {SyntaxLanguage::javascript, "// note\nconst value = \"ok\";\n"},
      {SyntaxLanguage::typescript, "interface A { value: string }\n"},
      {SyntaxLanguage::tsx, "const view = <div>ok</div>;\n"},
      {SyntaxLanguage::python, "# note\ndef value(): return 42\n"},
      {SyntaxLanguage::json, "{\"value\": 42}\n"},
      {SyntaxLanguage::yaml, "value: 42\n"},
      {SyntaxLanguage::toml, "value = 42\n"},
      {SyntaxLanguage::markdown, "# heading\n\ntext\n"},
      {SyntaxLanguage::shell, "# note\necho \"ok\"\n"},
      {SyntaxLanguage::cmake, "set(VALUE \"ok\")\n"}};
  for (const auto& [language, fixture] : grammar_fixtures) {
    syntax_error.clear();
    check(syntax.update(language, fixture, syntax_error) &&
              syntax.language() == language && !syntax.spans().empty(),
          "independently pinned tree-sitter grammar parses and highlights fixture");
  }

  MarkdownParser parser;
  const auto markdown = parser.parse("# Title\n\n[link](javascript:bad) **ok** <b>x</b>");
  const auto rml = markdown_to_safe_rml(markdown);
  check(!markdown.nodes.empty(), "markdown produces owned AST");
  check(rml.find("javascript:") == std::string::npos, "markdown removes unsafe link");
  check(rml.find("&lt;b&gt;") != std::string::npos, "markdown escapes raw HTML");
  const auto copy_rml = markdown_to_safe_rml_with_copy(
      parser.parse("before\n\n```cpp\nint answer = 42;\n```\n"), "copy-test");
  check(copy_rml.code_blocks.size() == 1 &&
            copy_rml.code_blocks.front().id == "copy-test-code-0" &&
            copy_rml.code_blocks.front().text == "int answer = 42;\n" &&
            copy_rml.rml.find("int answer = 42;") != std::string::npos &&
            copy_rml.rml.find("<pre class=\"code-block\" data-language=\"cpp\">") !=
                std::string::npos &&
            copy_rml.rml.find("data-copy-markdown=\"copy-test-code-0\"") !=
                std::string::npos,
        "Markdown code renders visibly and keeps exact copy text outside DOM attributes");
  const auto rich_markdown = parser.parse(
      "# Heading\n\n- [x] done\n\n> [!NOTE] safe\n\n"
      "[file](tokmon-file:src/sample.cpp) ![remote](https://example.com/x.png)\n\n"
      "```diff\n-old\n+new\n```\n\n```tool-call\n{\"name\":\"test\"}\n```\n");
  check(std::ranges::all_of(rich_markdown.nodes, [&](const auto& node) {
          return node.id != 0 && node.source.byte_start <= node.source.byte_end &&
                 (&node == &rich_markdown.nodes[rich_markdown.root] ||
                  node.parent < rich_markdown.nodes.size());
        }),
        "markdown AST has stable IDs, source ranges, and valid parents");
  check(std::ranges::any_of(rich_markdown.nodes, [](const auto& node) {
          return node.kind == MarkdownNodeKind::task_item && node.checked;
        }) && std::ranges::any_of(rich_markdown.nodes, [](const auto& node) {
          return node.kind == MarkdownNodeKind::callout;
        }) && std::ranges::any_of(rich_markdown.nodes, [](const auto& node) {
          return node.kind == MarkdownNodeKind::diff_block;
        }) && std::ranges::any_of(rich_markdown.nodes, [](const auto& node) {
          return node.kind == MarkdownNodeKind::tool_call;
        }),
        "markdown AST classifies task, callout, diff, and tool blocks");
  const auto rich_rml = markdown_to_safe_rml(rich_markdown);
  check(rich_rml.find("<img") == std::string::npos &&
            rich_rml.find("markdown-image-placeholder") != std::string::npos &&
            rich_rml.find("data-file=\"tokmon-file:src/sample.cpp\"") !=
                std::string::npos,
        "markdown uses explicit file references and non-fetching image placeholders");
  MarkdownStream stream;
  stream.append("# Stable\n\nfirst paragraph\n\n");
  const auto first_id = stream.document().nodes[
      stream.document().nodes[stream.document().root].children.front()].id;
  stream.append("second **streamed** paragraph");
  check(stream.reparsed_from() > 0 &&
            stream.document().nodes[
                stream.document().nodes[stream.document().root].children.front()].id ==
                first_id,
        "streaming markdown reparses only the open tail and preserves stable IDs");
  const std::string chunk_fixture =
      "# Chunked\n\nparagraph **one**\n\n- first\n- second\n\n"
      "```cpp\nint value = 42;\n```\n\nfinal [link](https://example.com)\n";
  const auto direct_chunk_rml = markdown_to_safe_rml(parser.parse(chunk_fixture));
  bool chunk_boundaries_match = true;
  for (const std::size_t chunk_size : {1u, 2u, 3u, 5u, 8u, 13u, 64u}) {
    MarkdownStream chunked;
    for (std::size_t offset = 0; offset < chunk_fixture.size();
         offset += chunk_size)
      chunked.append(std::string_view(chunk_fixture).substr(offset, chunk_size));
    chunk_boundaries_match = chunk_boundaries_match &&
        markdown_to_safe_rml(chunked.document()) == direct_chunk_rml;
  }
  check(chunk_boundaries_match,
        "streaming markdown is invariant across adversarial chunk boundaries");
  const auto huge_markdown = parser.parse(
      std::string(10u * 1024u * 1024u, 'x'));
  const auto bounded_rml = markdown_to_safe_rml(huge_markdown);
  check(huge_markdown.source_bytes == 10u * 1024u * 1024u &&
            bounded_rml.size() <= markdown_rml_text_limit + 4096u &&
            bounded_rml.find("markdown-truncated") != std::string::npos,
        "10 MiB markdown is parsed into owned AST but rendered through a bounded DOM payload");

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
  check(terminal_snapshot.default_background.red ==
                legacy_theme::surface_warm.red &&
            terminal_snapshot.default_background.green ==
                legacy_theme::surface_warm.green &&
            terminal_snapshot.default_background.blue ==
                legacy_theme::surface_warm.blue &&
            terminal_snapshot.default_foreground.red == legacy_theme::body.red &&
            terminal_snapshot.default_foreground.green ==
                legacy_theme::body.green &&
            terminal_snapshot.default_foreground.blue ==
                legacy_theme::body.blue &&
            terminal_snapshot.cursor_color.red == legacy_theme::accent.red &&
            terminal_snapshot.cursor_color.green == legacy_theme::accent.green &&
            terminal_snapshot.cursor_color.blue == legacy_theme::accent.blue,
        "libghostty-vt uses the Forest Sage warm-light pane palette");
  check(vt.encode_key(TerminalKey::unidentified, "x", 0) == "x",
        "libghostty-vt encodes raw UTF-8 terminal text input");
  check(vt.encode_key(TerminalKey::enter, {}, 0) == "\r",
        "libghostty-vt encodes terminal Enter key");
  check(!vt.encode_key(TerminalKey::left, {}, terminal_ctrl).empty() &&
            !vt.encode_key(TerminalKey::f5, {}, terminal_shift | terminal_alt)
                 .empty(),
        "libghostty-vt encodes modified navigation and function keys");
  vt.append("\x1b[?1000h\x1b[?1006h");
  check(vt.mouse_tracking() &&
            !vt.encode_mouse(TerminalMouseAction::press,
                             TerminalMouseButton::left, 16.f, 16.f,
                             320, 80, 8, 16, terminal_ctrl).empty(),
        "libghostty-vt encodes SGR mouse tracking with modifiers");
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
  GhosttyVt colour_vt(24, 3);
  colour_vt.append("\x1b[38;2;12;34;56m真彩色\x1b[0m e\xcc\x81 👨‍👩‍👧‍👦");
  const auto colour_snapshot = colour_vt.render_snapshot();
  check(std::ranges::any_of(colour_snapshot.cells, [](const auto& cell) {
          return cell.foreground.red == 12 && cell.foreground.green == 34 &&
                 cell.foreground.blue == 56;
        }) && colour_vt.plain_text().find("真彩色") != std::string::npos,
        "libghostty-vt preserves truecolor and Unicode terminal graphemes");
  check(link_vt.resize(120, 40, 8, 16),
        "libghostty-vt resizes and reflows terminal grid");
  GhosttyVt alternate_vt(20, 3, 1000);
  alternate_vt.append("primary\x1b[?1049halternate\x1b[?1049l");
  check(alternate_vt.plain_text().find("primary") != std::string::npos &&
            alternate_vt.plain_text().find("alternate") == std::string::npos,
        "libghostty-vt restores the primary buffer after alternate-screen TUI use");
  const auto profiles = discover_terminal_profiles();
  check(!profiles.empty() && profiles.front().available &&
            !profiles.front().executable.empty(),
        "terminal discovers at least one native shell profile");
  check(resolve_terminal_profile("auto").has_value(),
        "terminal resolves the portable automatic profile");
#if defined(_WIN32)
  const auto has_profile = [&](const std::string_view id) {
    return std::ranges::find(profiles, id, &TerminalProfile::id) !=
        profiles.end();
  };
  check(has_profile("pwsh") && has_profile("windows-powershell") &&
            has_profile("cmd") && has_profile("wsl"),
        "Windows terminal profile catalog covers PowerShell, cmd, and WSL with availability flags");
#else
  const auto has_profile = [&](const std::string_view id) {
    return std::ranges::find(profiles, id, &TerminalProfile::id) !=
        profiles.end();
  };
  check(has_profile("zsh") && has_profile("bash") && has_profile("fish") &&
            has_profile("sh"),
        "Unix terminal profile catalog covers zsh, bash, fish, and sh with availability flags");
#endif
  std::string terminal_argument_error;
  const auto terminal_arguments = parse_terminal_arguments(
      R"(-NoLogo --name "Tokmon Desk" 'C:\work tree' escaped\ value)",
      terminal_argument_error);
  check(terminal_arguments && terminal_arguments->size() == 5 &&
            (*terminal_arguments)[0] == "-NoLogo" &&
            (*terminal_arguments)[2] == "Tokmon Desk" &&
            (*terminal_arguments)[3] == "C:\\work tree" &&
            (*terminal_arguments)[4] == "escaped value",
        "custom terminal arguments preserve quoted and escaped values");
  check(!parse_terminal_arguments("--name 'unfinished", terminal_argument_error) &&
            !terminal_argument_error.empty(),
        "custom terminal arguments reject unterminated quotes");
  TerminalRenderSnapshot selection_snapshot;
  selection_snapshot.columns = 4;
  selection_snapshot.rows = 2;
  selection_snapshot.cells.resize(8);
  selection_snapshot.cells[0].grapheme = "你";
  selection_snapshot.cells[1].grapheme = "好";
  selection_snapshot.cells[4].grapheme = "e\xcc\x81";
  selection_snapshot.cells[5].grapheme = "!";
  check(terminal_selection_text(selection_snapshot, 0, 7) ==
            "你好\ne\xcc\x81!",
        "terminal selection retains CJK and combining graphemes across lines");
  check(terminal_safe_hyperlink("https://example.com/path") &&
            terminal_safe_hyperlink("http://127.0.0.1") &&
            !terminal_safe_hyperlink("javascript:alert(1)") &&
            !terminal_safe_hyperlink("file:///etc/passwd") &&
            !terminal_safe_hyperlink("HTTPS://example.com"),
        "terminal OSC 8 scheme whitelist rejects unsafe and ambiguous URIs");

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
    check(terminal.resize(100, 30, terminal_error),
          "platform PTY accepts live resize");
    terminal.stop();
    check(terminal_echo && !terminal.running() &&
              !terminal.write("after stop", terminal_error),
          "platform PTY returns shell output and cleans up EOF/exit state");
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
  const auto before_navigation_remove = navigation.items().size();
  check(navigation.remove_selected_session() &&
            navigation.items().size() + 1 == before_navigation_remove &&
            navigation.selected() &&
            navigation.selected()->kind == "project",
        "navigation context deletion removes only the selected session and "
        "returns selection to its project");

  const auto app_paths = DeskAppPaths::resolve();
  check(app_paths.isolated_from(root),
        "desktop data roots are isolated from workspace and .tokmon");

  const auto desk_root = std::filesystem::path(root.string() + "-desk-profile");
  const auto isolated_paths = DeskAppPaths::resolve(desk_root);
  check(isolated_paths.config == desk_root / "config" &&
            isolated_paths.runtime == desk_root / "state" / "runtime" &&
            isolated_paths.change_snapshots ==
                desk_root / "data" / "change-snapshots",
        "Desktop accepts an explicit isolated app-data root");
  check(isolated_paths.ensure(error), "Desktop creates all isolated state roots");
  write_file(isolated_paths.cache / "old.cache", std::string(48, 'c'));
  write_file(isolated_paths.logs / "old.log", std::string(48, 'l'));
  write_file(isolated_paths.recovery / "old.recovery", std::string(48, 'r'));
  DeskRetentionPolicy tiny_retention;
  tiny_retention.cache_bytes = 16;
  tiny_retention.log_bytes = 16;
  tiny_retention.recovery_bytes = 16;
  tiny_retention.log_age = std::chrono::hours::max();
  tiny_retention.recovery_age = std::chrono::hours::max();
  DeskRetentionReport retention_report;
  check(isolated_paths.enforce_retention(tiny_retention, retention_report,
                                         error) &&
            retention_report.removed_files == 3 &&
            directory_bytes(isolated_paths.cache) <= 16 &&
            directory_bytes(isolated_paths.logs) <= 16 &&
            directory_bytes(isolated_paths.recovery) <= 16,
        "Desktop cache/log/recovery retention enforces independent quotas");
  write_file(root / ".tokmon" / "config.yaml", "model: fixture\n");
  const auto daemon_config_hash =
      tokmon::sha256_hex(read_file(root / ".tokmon" / "config.yaml"));
  DeskStateStore state_store(isolated_paths);
  const auto local_settings = tokmon::cbor::object(
      {{"language", "zh-CN"}, {"terminal_profile", "auto"}});
  const tokmon::cbor::Value local_navigation = tokmon::cbor::Value::Array{
      tokmon::cbor::object({{"id", "local"}, {"kind", "group"}})};
  check(state_store.save_settings(local_settings, error) &&
            state_store.save_navigation(local_navigation, error),
        "Desktop atomically persists local settings and navigation");
  std::string state_warning;
  const auto loaded_settings = state_store.load_settings(state_warning);
  const auto loaded_navigation = state_store.load_navigation(state_warning);
  check(field(loaded_settings, "language") &&
            field(loaded_settings, "language")->as_string() == "zh-CN" &&
            loaded_navigation.as_array() && loaded_navigation.as_array()->size() == 1,
        "Desktop reloads schema-versioned local state");
  check(tokmon::sha256_hex(read_file(root / ".tokmon" / "config.yaml")) ==
            daemon_config_hash,
        "Desktop-local persistence does not mutate workspace .tokmon state");
  {
    DeskInstanceLock first(isolated_paths.runtime / "tokmon-desk.lock");
    DeskInstanceLock second(isolated_paths.runtime / "tokmon-desk.lock");
    check(first.acquired() && !second.acquired(),
          "Desktop single-instance lock rejects a competing process handle");
  }
  write_file(isolated_paths.config / "settings.json", "{broken");
  const auto fallback_settings = state_store.load_settings(state_warning);
  check(fallback_settings.as_map() && fallback_settings.as_map()->empty() &&
            !state_warning.empty(),
        "Desktop quarantines corrupt local state and returns safe defaults");
  bool quarantined = false;
  for (const auto& item : std::filesystem::directory_iterator(isolated_paths.config))
    quarantined = quarantined ||
        item.path().filename().string().starts_with("settings.json.corrupt-");
  check(quarantined, "Desktop retains corrupt state as a diagnostic quarantine");

#ifdef TOKMON_DESK_SOURCE_DIR
  const auto desk_source = std::filesystem::path(TOKMON_DESK_SOURCE_DIR);
  const auto legacy_source = desk_source.parent_path() / "tokmon-desktop";
  const auto manifest_text = read_file(
      desk_source / "assets" / "visual-baseline-manifest.json");
  const auto manifest = tokmon::json::parse(manifest_text);
  check(manifest && manifest->as_map() &&
            field(*manifest, "schema") && field(*manifest, "schema")->as_integer() == 3,
        "visual baseline manifest is valid schema 3 JSON");
  if (manifest) {
    const auto* legacy = field(*manifest, "legacyDesktop");
    const auto* assets = field(*manifest, "assetsSha256");
    check(legacy && assets && assets->as_map() && assets->as_map()->size() == 65,
          "visual baseline freezes all 65 legacy icon and starter assets");
    if (legacy) {
      check(field(*legacy, "themeSha256") &&
                tokmon::sha256_hex(read_file(legacy_source / "ui" /
                                               "tokmon-theme.slint")) ==
                    field(*legacy, "themeSha256")->as_string(),
            "visual baseline matches the exact legacy theme source");
      check(field(*legacy, "mainUiSha256") &&
                tokmon::sha256_hex(read_file(legacy_source / "ui" / "tokmon.slint")) ==
                    field(*legacy, "mainUiSha256")->as_string(),
            "visual baseline matches the exact legacy desktop UI source");
    }
    if (assets && assets->as_map()) {
      bool exact_assets = true;
      for (const auto& [name, digest] : *assets->as_map()) {
        const auto new_file = desk_source / "assets" / "figma" / name;
        exact_assets = exact_assets && std::filesystem::is_regular_file(new_file) &&
            tokmon::sha256_hex(read_file(new_file)) == digest.as_string();
      }
      check(exact_assets,
            "tokmon-desk packaged SVGs match the Forest Sage asset manifest");
    }
    const auto* font = field(*manifest, "font");
    check(font && field(*font, "sha256") &&
              tokmon::sha256_hex(read_file(TOKMON_DESK_FONT_FILE)) ==
                  field(*font, "sha256")->as_string(),
          "packaged MiSans variable font matches the frozen visual baseline");
  }
  const auto dependency_manifest = tokmon::json::parse(read_file(
      desk_source / "assets" / "dependency-manifest.json"));
  check(dependency_manifest && dependency_manifest->as_map() &&
            field(*dependency_manifest, "schema") &&
            field(*dependency_manifest, "schema")->as_integer() == 1 &&
            field(*dependency_manifest, "direct") &&
            field(*dependency_manifest, "treeSitterGrammars") &&
            field(*dependency_manifest, "transitiveVcpkg"),
        "dependency manifest records direct, grammar, and transitive pins");
  if (dependency_manifest) {
    const auto* browser = field(*dependency_manifest, "browser");
    check(browser && field(*browser, "status") &&
              field(*browser, "status")->as_string() == "DEFERRED-BROWSER" &&
              field(*browser, "runtimeIncluded") &&
              !field(*browser, "runtimeIncluded")->as_bool(),
          "base dependency manifest explicitly excludes deferred Browser runtime");
  }

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
  const auto branch_before_dirty_test = git.status().branch;
  const auto dirty_branch_text = read_file(cpp_file) + "// dirty checkout guard\n";
  write_file(cpp_file, dirty_branch_text);
  error.clear();
  const bool dirty_checkout = git.checkout_branch("desk-branch", error);
  const bool dirty_preserved = read_file(cpp_file) == dirty_branch_text;
  const bool safely_refused = !dirty_checkout && !error.empty() &&
      git.status().branch == branch_before_dirty_test;
  bool returned_from_dirty_checkout = true;
  if (dirty_checkout) {
    error.clear();
    returned_from_dirty_checkout = git.checkout_branch("main", error) &&
        read_file(cpp_file) == dirty_branch_text;
  }
  check(dirty_preserved && (safely_refused || returned_from_dirty_checkout),
        "libgit2 dirty checkout either preserves compatible changes or safely refuses without loss");
  check(git.discard_file("src/sample.cpp",
                         DocumentStore::content_hash(dirty_branch_text), false,
                         error),
        "dirty branch checkout fixture restores the tracked file safely");
  const auto revision_before_commit = git.head_revision(error);
  write_file(root / "src" / "commit-fixture.txt", "commit through libgit2\n");
  check(git.stage_file("src/commit-fixture.txt", error) &&
            git.commit("tokmon-desk commit fixture", error) &&
            git.head_revision(error) != revision_before_commit,
        "libgit2 stages and commits with a diagnostic revision change");
  std::string push_error;
  check(!git.push(push_error) && !push_error.empty(),
        "libgit2 push without a remote reports a diagnostic failure");
  check(git.diff_model("../outside.txt", false, error) == std::nullopt,
        "Git operations reject workspace path escape");

  DesktopChangeTracker tracker(root, isolated_paths.change_snapshots);
  check(tracker.begin("agent-run-1", error),
        "Desktop ChangeTracker captures a pre-run Git baseline");
  const auto tracked_before = read_file(cpp_file);
  write_file(cpp_file, tracked_before + "// agent edit\n");
  write_file(root / "src" / "agent-created.txt", "agent output\n");
  auto changes = tracker.finish(error);
  if (!changes) {
    std::cerr << "ChangeSet finish error=" << error << '\n';
  }
  check(changes && changes->changes.size() == 2 &&
            std::ranges::all_of(changes->changes, [](const auto& change) {
              return change.reversible && !change.after_sha256.empty();
            }),
        "Desktop ChangeTracker precisely attributes tracked and created files");
  if (changes) {
    const auto duplicate_digest = tokmon::sha256_hex("deduplicated preimage\n");
    const auto duplicate_blob = isolated_paths.change_snapshots /
        duplicate_digest.substr(0, 2) / (duplicate_digest + ".blob");
    std::size_t duplicate_blob_count = 0;
    for (std::filesystem::recursive_directory_iterator iterator(
             isolated_paths.change_snapshots), end;
         iterator != end; ++iterator)
      if (iterator->is_regular_file() &&
          iterator->path().filename() == duplicate_blob.filename())
        ++duplicate_blob_count;
    check(duplicate_blob_count == 1 &&
              read_file(duplicate_blob) == "deduplicated preimage\n",
          "Desktop ChangeTracker content-addresses identical preimages once");
    const auto tracked_change = std::ranges::find_if(
        changes->changes, [](const auto& change) {
          return change.path == "src/sample.cpp";
        });
    if (tracked_change != changes->changes.end()) {
      const auto blob = isolated_paths.change_snapshots /
          tracked_change->preimage_blob.substr(0, 2) /
          (tracked_change->preimage_blob + ".blob");
      const auto exact_blob = read_file(blob);
      write_file(blob, "corrupted snapshot\n");
      auto corrupt_attempt = *changes;
      check(!tracker.reject(corrupt_attempt, error) &&
                error.find("integrity") != std::string::npos &&
                read_file(cpp_file) == tracked_before + "// agent edit\n",
            "Desktop ChangeTracker verifies content-addressed preimage integrity before restore");
      write_file(blob, exact_blob);
    }
    write_file(cpp_file, read_file(cpp_file) + "// user edit after run\n");
    check(!tracker.reject(*changes, error) &&
              read_file(cpp_file).find("user edit after run") != std::string::npos,
          "ChangeSet rejection refuses to overwrite a later user edit");
    write_file(cpp_file, tracked_before + "// agent edit\n");
    const auto rejected = tracker.reject(*changes, error);
    const auto restored_exactly = read_file(cpp_file) == tracked_before;
    const auto removed_created =
        !std::filesystem::exists(root / "src" / "agent-created.txt");
    if (!rejected || !restored_exactly || !removed_created)
      std::cerr << "ChangeTracker detail: rejected=" << rejected
                << " restored=" << restored_exactly
                << " removed=" << removed_created << " error=" << error << '\n';
    if (!restored_exactly)
      std::cerr << "ChangeTracker hashes: expected="
                << tokmon::sha256_hex(tracked_before) << " size="
                << tracked_before.size() << " actual="
                << tokmon::sha256_hex(read_file(cpp_file)) << " size="
                << read_file(cpp_file).size() << '\n';
    check(rejected && restored_exactly && removed_created,
          "ChangeSet rejection restores preimages and removes Agent-created files");
  }
  check(tracker.begin("agent-run-accept", error),
        "Desktop ChangeTracker starts a second independent run");
  write_file(cpp_file, tracked_before + "// accepted edit\n");
  auto accepted_changes = tracker.finish(error);
  const auto accepted = accepted_changes && tracker.accept(*accepted_changes, error);
  const auto post_accept_status = git.status();
  const auto accepted_file = std::ranges::find_if(
      post_accept_status.files, [](const auto& status) {
        return status.path == "src/sample.cpp";
      });
  check(accepted && accepted_changes->accepted &&
            accepted_file != post_accept_status.files.end() &&
            accepted_file->index_status == ' ' &&
            accepted_file->worktree_status != ' ',
        "accepting a ChangeSet preserves edits without implicitly staging them");
  const auto tiny_snapshot_root = isolated_paths.data / "tiny-snapshot-quota";
  DesktopChangeTracker quota_tracker(root, tiny_snapshot_root, 4);
  check(!quota_tracker.begin("quota-run", error) &&
            error.find("quota") != std::string::npos,
        "Desktop ChangeTracker enforces its content-addressed snapshot quota");
#endif

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::remove_all(desk_root, cleanup_error);
  std::filesystem::remove_all(recovery_root, cleanup_error);
  std::filesystem::remove_all(outside_directory, cleanup_error);
  if (failures == 0)
    std::cout << "tokmon-desk core tests passed\n";
  return failures == 0 ? 0 : 1;
}
