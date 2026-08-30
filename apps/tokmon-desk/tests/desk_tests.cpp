#include "app/app_state.hpp"
#include "browser/browser_manager.hpp"
#include "editor/document_store.hpp"
#include "editor/grapheme.hpp"
#include "editor/syntax_service.hpp"
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
#include "workspace/workspace_service.hpp"

#include "tokmon/hash.hpp"
#include "tokmon/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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
  write_file(root / ".gitignore", "ignored/\n*.cache\n");
  write_file(root / "ignored" / "secret.txt", "must stay hidden\n");
  write_file(root / "src" / "scratch.cache", "must stay hidden\n");

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
                 entry.relative_path.ends_with(".cache");
        });
  if (!ignored_hidden) {
    for (const auto& entry : async_entries)
      if (entry.relative_path.starts_with("ignored") ||
          entry.relative_path.ends_with(".cache"))
        std::cerr << "Ignore detail: " << entry.relative_path << '\n';
  }
  check(ignored_hidden,
        "workspace tree applies repository .gitignore rules");
  check(workspace.create_directory("generated", error),
        "workspace creates a contained directory");
  check(workspace.create_file("generated/new.txt", "new\n", error),
        "workspace creates a contained file");
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
  check(crlf && crlf->encoding == TextEncoding::utf8_bom &&
            crlf->line_ending == LineEnding::crlf,
        "document reports BOM encoding and CRLF metadata");
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

  const auto desk_root = std::filesystem::path(root.string() + "-desk-profile");
  DeskAppPaths isolated_paths{
      .config = desk_root / "config", .data = desk_root / "data",
      .state = desk_root / "state", .cache = desk_root / "cache",
      .logs = desk_root / "logs", .runtime = desk_root / "runtime",
      .recovery = desk_root / "state" / "recovery",
      .change_snapshots = desk_root / "data" / "change-snapshots"};
  check(isolated_paths.ensure(error), "Desktop creates all isolated state roots");
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
            field(*manifest, "schema") && field(*manifest, "schema")->as_integer() == 2,
        "visual baseline manifest is valid schema 2 JSON");
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
        const auto old_file = legacy_source / "assets" / "figma" / name;
        const auto new_file = desk_source / "assets" / "figma" / name;
        exact_assets = exact_assets && std::filesystem::is_regular_file(old_file) &&
            std::filesystem::is_regular_file(new_file) &&
            tokmon::sha256_hex(read_file(old_file)) == digest.as_string() &&
            tokmon::sha256_hex(read_file(new_file)) == digest.as_string();
      }
      check(exact_assets,
            "tokmon-desk preserves every legacy SVG byte-for-byte");
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
#endif

  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::remove_all(desk_root, cleanup_error);
  std::filesystem::remove_all(recovery_root, cleanup_error);
  if (failures == 0)
    std::cout << "tokmon-desk core tests passed\n";
  return failures == 0 ? 0 : 1;
}
