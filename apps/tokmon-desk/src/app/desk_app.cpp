#include "app/desk_app.hpp"

#include "integration/daemon_client.hpp"
#include "fonts/font_manager.hpp"
#include "platform/desk_app_paths.hpp"
#include "platform/sdl_platform.hpp"
#include "render/rml_render_interface_skia.hpp"
#include "render/skia_device.hpp"
#include "ui/desk_controller.hpp"
#include "ui/elements/element_code_surface.hpp"
#include "ui/elements/element_diff_surface.hpp"
#include "ui/elements/element_file_tree.hpp"
#include "ui/elements/element_terminal.hpp"

#include "tokmon/config.hpp"
#include "tokmon/daemon_lifecycle.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace tokmon::desk {
namespace {

struct Arguments {
  std::filesystem::path workspace = std::filesystem::current_path();
  bool smoke_test{false};
  bool software_renderer{false};
  int ui_scale_percent{0};
  std::filesystem::path screenshot;
  std::filesystem::path acceptance_report;
};

struct BackendConnection {
  std::optional<tokmon::DaemonConnection> connection;
  std::optional<tokmon::DaemonClientLease> lease;
  std::string error;
};

Arguments parse_arguments(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--workspace" && index + 1 < argc)
      result.workspace = std::filesystem::absolute(argv[++index]);
    else if (argument == "--smoke-test")
      result.smoke_test = true;
    else if (argument == "--software-renderer")
      result.software_renderer = true;
    else if (argument == "--ui-scale" && index + 1 < argc)
      result.ui_scale_percent = std::clamp(std::stoi(argv[++index]), 70, 200);
    else if (argument == "--screenshot" && index + 1 < argc)
      result.screenshot = std::filesystem::absolute(argv[++index]);
    else if (argument == "--acceptance-report" && index + 1 < argc)
      result.acceptance_report = std::filesystem::absolute(argv[++index]);
  }
  return result;
}

std::filesystem::path resource_root() {
  if (const char* base = SDL_GetBasePath()) {
    const std::filesystem::path candidate(base);
    if (std::filesystem::exists(candidate / "rml" / "documents" / "main.rml"))
      return candidate;
  }
#ifdef TOKMON_DESK_SOURCE_DIR
  const std::filesystem::path source(TOKMON_DESK_SOURCE_DIR);
  if (std::filesystem::exists(source / "rml" / "documents" / "main.rml"))
    return source;
#endif
  return std::filesystem::current_path() / "apps" / "tokmon-desk";
}

void set_text(Rml::ElementDocument& document, const char* id, const std::string& value) {
  if (auto* element = document.GetElementById(id))
    element->SetInnerRML(value);
}

SDL_HitTestResult hit_test(SDL_Window* window, const SDL_Point* point, void*) {
  (void)window;
  // Keep the dedicated brand strip draggable, but never place a native drag
  // hit-test region over RmlUi title-bar controls. SDL consumes those pointer
  // events before RmlUi can dispatch them.
  if (point->y < 42 && point->x >= 30 && point->x < 210)
    return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}

} // namespace

int run_desk(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto paths = DeskAppPaths::resolve();
    std::string error;
    if (!paths.ensure(error)) {
      std::cerr << "tokmon-desk: " << error << '\n';
      return 2;
    }
    if (!paths.isolated_from(arguments.workspace)) {
      std::cerr << "tokmon-desk: application data paths overlap the workspace/.tokmon\n";
      return 2;
    }

    SdlPlatform platform;
    if (!platform.initialize("Tokmon", 1440, 900, error)) {
      std::cerr << "tokmon-desk: SDL initialization failed: " << error << '\n';
      return 3;
    }
    SDL_SetWindowHitTest(platform.window(), hit_test, nullptr);

    const int ui_scale_percent = arguments.ui_scale_percent > 0
                                     ? arguments.ui_scale_percent
                                     : platform.default_ui_scale_percent();
    const float ui_scale = static_cast<float>(ui_scale_percent) / 100.f;
    platform.set_ui_scale(ui_scale);
    platform.size_window_for_ui_scale(1440, 900);
    auto device = SkiaDevice::create(platform.window(), platform.pixel_width(),
                                     platform.pixel_height(),
                                     arguments.software_renderer, error);
    if (!device) {
      std::cerr << "tokmon-desk: Skia initialization failed: " << error << '\n';
      return 4;
    }
    device->set_ui_scale(ui_scale);
    std::cerr << "tokmon-desk: renderer=" << device->backend_name()
              << " ui-scale=" << ui_scale_percent << "%\n";
    RmlRenderInterfaceSkia renderer(*device);
    Rml::SetSystemInterface(&platform);
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise()) {
      std::cerr << "tokmon-desk: RmlUi initialization failed\n";
      return 5;
    }
    register_terminal_element();
    register_code_surface_element();
    register_diff_surface_element();
    register_file_tree_element();

    const auto resources = resource_root();
    const auto font = resources / "assets" / "fonts" / "MiSansVF.ttf";
    FontManager font_manager;
    if (!font_manager.load_ui_font(font, error) ||
        font_manager.shape_utf8("Tokmon 中文 Aa 123", 13.f).empty()) {
      std::cerr << "tokmon-desk: HarfBuzz/FreeType font validation failed: "
                << error << '\n';
      Rml::Shutdown();
      return 6;
    }
    if (!Rml::LoadFontFace(font.generic_string(), true)) {
      std::cerr << "tokmon-desk: required MiSans font not found: " << font << '\n';
      Rml::Shutdown();
      return 6;
    }
#if defined(_WIN32)
    const std::filesystem::path terminal_font = "C:/Windows/Fonts/consola.ttf";
    for (const auto& fallback : {std::filesystem::path("C:/Windows/Fonts/seguisym.ttf"),
                                 std::filesystem::path("C:/Windows/Fonts/seguiemj.ttf")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#elif defined(__APPLE__)
    const std::filesystem::path terminal_font = "/System/Library/Fonts/Menlo.ttc";
    for (const auto& fallback : {std::filesystem::path("/System/Library/Fonts/Apple Symbols.ttf"),
                                 std::filesystem::path("/System/Library/Fonts/Apple Color Emoji.ttc")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#else
    const std::filesystem::path terminal_font = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    for (const auto& fallback : {std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
                                 std::filesystem::path("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#endif
    if (std::filesystem::exists(terminal_font))
      (void)Rml::LoadFontFace(terminal_font.generic_string(), false);
    auto* context = Rml::CreateContext("tokmon-desk",
        {device->logical_width(), device->logical_height()}, nullptr);
    if (!context) {
      std::cerr << "tokmon-desk: could not create RmlUi context\n";
      Rml::Shutdown();
      return 7;
    }
    context->SetDensityIndependentPixelRatio(platform.display_scale());
    auto* document = context->LoadDocument(
        (resources / "rml" / "documents" / "main.rml").generic_string());
    if (!document) {
      std::cerr << "tokmon-desk: could not load main.rml\n";
      Rml::Shutdown();
      return 8;
    }
    document->Show();
    std::filesystem::path daemon_endpoint;
    if (auto resolved = tokmon::resolve_paths(arguments.workspace); resolved)
      daemon_endpoint = tokmon::workspace_snow_endpoint(
          resolved->run, resolved->project.parent_path());
    DeskController controller(*document, platform, arguments.workspace, paths.data,
                              daemon_endpoint);
    controller.bind();

    if (!arguments.acceptance_report.empty()) {
      auto* tree = dynamic_cast<ElementFileTree*>(
          document->GetElementById("file-tree"));
      auto* editor = dynamic_cast<ElementCodeSurface*>(
          document->GetElementById("file-preview"));
      auto* files_view = document->GetElementById("files-view");
      auto* review_view = document->GetElementById("review-view");
      auto* review_diff = document->GetElementById("review-diff");
      if (!tree || !editor || !files_view || !review_view || !review_diff) {
        std::cerr << "tokmon-desk: acceptance elements are missing\n";
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      document->GetElementById("app-shell")->SetClass("right-fullscreen", true);
      files_view->SetClass("hidden", false);
      review_view->SetClass("hidden", true);
      std::vector<WorkspaceEntry> rows;
      rows.reserve(10000);
      for (std::size_t index = 0; index < 10000; ++index)
        rows.push_back({arguments.workspace / "large-tree" /
                            ("file-" + std::to_string(index) + ".txt"),
                        "large-tree/file-" + std::to_string(index) + ".txt",
                        "file-" + std::to_string(index) + ".txt",
                        index % 4, false, false});
      tree->set_rows(std::move(rows));
      std::string large_document;
      large_document.reserve(2u * 1024u * 1024u);
      for (std::size_t index = 0; index < 100000; ++index)
        large_document += "virtual editor line " + std::to_string(index) + "\n";
      editor->set_document(std::move(large_document), {}, 1);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance file-tree present failed: "
                  << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto tree_rendered = tree->visible_geometry_rows();
      const auto tree_dom_children = tree->GetNumChildren();
      const auto editor_lines = editor->line_count();
      const auto editor_rendered = editor->rendered_line_count();
      const auto editor_dom_children = editor->GetNumChildren();

      review_diff->SetInnerRML(
          "<tokmon-diff-surface id='acceptance-diff' class='diff-surface'>"
          "</tokmon-diff-surface>");
      auto* diff = dynamic_cast<ElementDiffSurface*>(
          document->GetElementById("acceptance-diff"));
      if (!diff) {
        std::cerr << "tokmon-desk: acceptance diff element is missing\n";
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      GitFileDiff model;
      model.path = "large-diff.txt";
      GitDiffHunk hunk;
      hunk.header = "@@ -1,4100 +1,4100 @@";
      hunk.lines.reserve(4100);
      for (int line = 1; line <= 4100; ++line)
        hunk.lines.push_back({line % 3 == 0 ? '+' : ' ', line, line,
                              "virtual diff line " + std::to_string(line)});
      model.hunks.push_back(std::move(hunk));
      diff->set_diff(std::move(model));
      files_view->SetClass("hidden", true);
      review_view->SetClass("hidden", false);
      review_diff->SetClass("hidden", false);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance diff present failed: " << error
                  << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto diff_lines = diff->line_count();
      const auto diff_rendered = diff->rendered_line_count();
      const auto diff_dom_children = diff->GetNumChildren();
      diff->set_split_view(true);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance split diff present failed: "
                  << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto split_diff_rendered = diff->rendered_line_count();
      int window_width = 0;
      int window_height = 0;
      SDL_GetWindowSize(platform.window(), &window_width, &window_height);
      std::error_code report_error;
      std::filesystem::create_directories(
          arguments.acceptance_report.parent_path(), report_error);
      std::ofstream report(arguments.acceptance_report,
                           std::ios::binary | std::ios::trunc);
      report << "{\n"
             << "  \"renderer\": \"" << device->backend_name() << "\",\n"
             << "  \"windowWidth\": " << window_width << ",\n"
             << "  \"windowHeight\": " << window_height << ",\n"
             << "  \"pixelWidth\": " << platform.pixel_width() << ",\n"
             << "  \"pixelHeight\": " << platform.pixel_height() << ",\n"
             << "  \"pixelDensity\": "
             << SDL_GetWindowPixelDensity(platform.window()) << ",\n"
             << "  \"displayScale\": " << platform.display_scale() << ",\n"
             << "  \"uiScalePercent\": " << ui_scale_percent << ",\n"
             << "  \"rmlWidth\": " << device->logical_width() << ",\n"
             << "  \"rmlHeight\": " << device->logical_height() << ",\n"
             << "  \"fileTreeModelRows\": 10000,\n"
             << "  \"fileTreeRenderedRows\": " << tree_rendered << ",\n"
             << "  \"fileTreeDomChildren\": " << tree_dom_children << ",\n"
             << "  \"editorModelLines\": " << editor_lines << ",\n"
             << "  \"editorRenderedLines\": " << editor_rendered << ",\n"
             << "  \"editorClientWidth\": " << editor->GetClientWidth() << ",\n"
             << "  \"editorClientHeight\": " << editor->GetClientHeight() << ",\n"
             << "  \"editorDomChildren\": " << editor_dom_children << ",\n"
             << "  \"diffModelLines\": " << diff_lines << ",\n"
             << "  \"diffRenderedLines\": " << diff_rendered << ",\n"
             << "  \"diffClientWidth\": " << diff->GetClientWidth() << ",\n"
             << "  \"diffClientHeight\": " << diff->GetClientHeight() << ",\n"
             << "  \"diffDomChildren\": " << diff_dom_children << ",\n"
             << "  \"splitDiffRenderedLines\": " << split_diff_rendered
             << "\n"
             << "}\n";
      const bool accepted = report && tree_rendered > 0 && tree_rendered < 200 &&
                            tree_dom_children == 0 && editor_lines >= 100000 &&
                            editor_rendered > 0 && editor_rendered < 200 &&
                            editor_dom_children == 0 && diff_lines >= 4000 &&
                            diff_rendered > 0 && diff_rendered < 200 &&
                            diff_dom_children == 0 && split_diff_rendered > 0 &&
                            split_diff_rendered < 200;
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return accepted ? 0 : 11;
    }

    std::future<BackendConnection> daemon_probe;
    std::optional<tokmon::DaemonClientLease> daemon_lease;
    if (!daemon_endpoint.empty()) {
      const auto daemon_executable =
#if defined(_WIN32)
          std::filesystem::path(SDL_GetBasePath()) / "tokmon.exe";
#else
          std::filesystem::path(SDL_GetBasePath()) / "tokmon";
#endif
      daemon_probe = std::async(std::launch::async,
          [daemon_endpoint, daemon_executable,
           workspace = arguments.workspace] {
        BackendConnection backend;
        auto connection = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
            .endpoint = daemon_endpoint,
            .workspace = workspace,
            .executable = daemon_executable});
        if (!connection) {
          backend.error = connection.error().describe();
          return backend;
        }
        backend.connection = std::move(*connection);
        auto lease = tokmon::DaemonClientLease::attach(
            tokmon::DaemonClientOptions{
                .endpoint = daemon_endpoint,
                .client_id = tokmon::make_id("tokmon-desk-client"),
                .client_kind = "desktop",
                .shutdown_when_idle = true,
                .idle_timeout = std::chrono::milliseconds(250),
                .lease_ttl = std::chrono::seconds(6)});
        if (!lease) {
          backend.error = lease.error().describe();
          return backend;
        }
        backend.lease = std::move(*lease);
        return backend;
      });
    } else {
      set_text(*document, "daemon-status", "当前工作区未配置 Tokmon");
    }

    bool quit = false;
    bool screenshot_written = false;
    int frames = 0;
    bool redraw = true;
    while (!quit && !controller.quit_requested()) {
      const bool warmup = frames < 3;
      if (!redraw && !warmup && !arguments.smoke_test)
        (void)platform.wait_for_event(controller.update_poll_interval_ms());
      bool resized = false;
      bool event_received = false;
      while (platform.pump_event(*context, quit, resized))
        event_received = true;
      if (resized && !device->resize(platform.pixel_width(), platform.pixel_height(), error)) {
        std::cerr << "tokmon-desk: resize failed: " << error << '\n';
        break;
      }
      if (resized)
        context->SetDimensions({device->logical_width(), device->logical_height()});
      redraw = redraw || event_received || resized || controller.update();
      if (daemon_probe.valid() &&
          daemon_probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        redraw = true;
        auto backend = daemon_probe.get();
        if (!backend.error.empty()) {
          set_text(*document, "daemon-status", "后台服务连接失败");
          std::cerr << "tokmon-desk: daemon connection failed: "
                    << backend.error << '\n';
        } else {
          const bool started = backend.connection && backend.connection->started;
          daemon_lease = std::move(backend.lease);
          set_text(*document, "daemon-status",
                   started ? "后台服务已启动；正在检查配置"
                           : "后台服务已连接；正在检查配置");
          controller.backend_connected();
        }
      }
      if (redraw || warmup || arguments.smoke_test) {
        context->Update();
        device->begin_frame();
        context->Render();
        if (!device->end_frame(error)) {
          std::cerr << "tokmon-desk: present failed: " << error << '\n';
          break;
        }
        ++frames;
        redraw = false;
      }
      if (!arguments.screenshot.empty() && !screenshot_written && frames >= 3) {
        screenshot_written = device->save_png(arguments.screenshot, error);
        if (!screenshot_written)
          std::cerr << "tokmon-desk: " << error << '\n';
      }
      if (arguments.smoke_test && frames >= 8)
        quit = true;
    }

    Rml::RemoveContext("tokmon-desk");
    Rml::Shutdown();
    return arguments.smoke_test && !arguments.screenshot.empty() && !screenshot_written ? 9 : 0;
  } catch (const std::exception& exception) {
    std::cerr << "tokmon-desk: fatal: " << exception.what() << '\n';
    return 10;
  }
}

} // namespace tokmon::desk
