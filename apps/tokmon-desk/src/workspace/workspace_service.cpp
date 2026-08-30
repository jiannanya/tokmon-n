#include "workspace/workspace_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>

#if defined(TOKMON_DESK_HAS_LIBGIT2)
#include <git2.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#endif

namespace tokmon::desk {
namespace {

std::string utf8(const std::filesystem::path& path) {
  // Workspace-facing paths are protocol/UI paths and therefore always use
  // forward slashes. libgit2's ignore matcher also requires Git-style paths
  // on Windows.
  const auto value = path.generic_u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool ignored(const std::filesystem::path& path) {
  const auto name = path.filename();
  return name == ".git" || name == ".tokmon" || name == "node_modules" ||
         name == "build" || name == "vcpkg_installed";
}

bool ignored_relative(const std::filesystem::path& path) {
  for (const auto& component : path)
    if (ignored(component))
      return true;
  return false;
}

class IgnoreRules final {
 public:
  explicit IgnoreRules(const std::filesystem::path& root) : root_(root) {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
    git_libgit2_init();
    git_repository* opened = nullptr;
    if (git_repository_open_ext(&opened, root.string().c_str(),
                                GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr) == 0)
      repository_ = opened;
#endif
  }
  ~IgnoreRules() {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
    if (repository_)
      git_repository_free(repository_);
    git_libgit2_shutdown();
#endif
  }
  [[nodiscard]] bool ignored_path(const std::filesystem::path& absolute) const {
    std::error_code error;
    const auto relative = std::filesystem::relative(absolute, root_, error);
    if (error || ignored_relative(relative))
      return true;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
    if (repository_) {
      int ignored_by_git = 0;
      auto git_path = utf8(relative);
      std::error_code type_error;
      if (std::filesystem::is_directory(absolute, type_error) &&
          !git_path.ends_with('/'))
        git_path.push_back('/');
      if (git_status_should_ignore(&ignored_by_git, repository_,
                                   git_path.c_str()) == 0 &&
          ignored_by_git != 0)
        return true;
    }
#endif
    return false;
  }

 private:
  std::filesystem::path root_;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_repository* repository_{nullptr};
#endif
};

std::string ascii_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

struct FileStamp {
  std::filesystem::file_time_type write_time{};
  std::uintmax_t size{0};
  bool directory{false};

  bool operator==(const FileStamp&) const = default;
};

using StampMap = std::unordered_map<std::string, FileStamp>;

StampMap collect_stamps(const std::filesystem::path& root) {
  StampMap result;
  if (root.empty())
    return result;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  const IgnoreRules ignore_rules(root);
  for (; !error && iterator != end; iterator.increment(error)) {
    const auto& entry = *iterator;
    if (ignore_rules.ignored_path(entry.path())) {
      if (entry.is_directory(error))
        iterator.disable_recursion_pending();
      error.clear();
      continue;
    }
    const bool directory = entry.is_directory(error);
    const auto relative = std::filesystem::relative(entry.path(), root, error);
    if (error) {
      error.clear();
      continue;
    }
    const auto write_time = entry.last_write_time(error);
    if (error)
      error.clear();
    const auto size = directory ? 0 : entry.file_size(error);
    if (error)
      error.clear();
    result.emplace(utf8(relative), FileStamp{write_time, size, directory});
  }
  return result;
}

} // namespace

struct WorkspaceWatcher::Impl {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::filesystem::path root;
  std::filesystem::path ready_root;
  StampMap stamps;
  std::vector<WorkspaceChange> pending;
  std::jthread worker;

  Impl() : worker([this](std::stop_token stop) { run(stop); }) {}

  void publish(std::vector<WorkspaceChange> changes,
               const std::filesystem::path& watched_root) {
    std::scoped_lock lock(mutex);
    if (watched_root != root)
      return;
    for (auto& change : changes) {
      const auto duplicate = std::find_if(
          pending.begin(), pending.end(), [&](const WorkspaceChange& item) {
            return item.path == change.path && item.kind == change.kind;
          });
      if (duplicate == pending.end())
        pending.push_back(std::move(change));
    }
  }

#if defined(__APPLE__)
  static void fsevents_callback(ConstFSEventStreamRef, void* context,
                                std::size_t count, void* paths,
                                const FSEventStreamEventFlags flags[],
                                const FSEventStreamEventId[]) {
    auto* self = static_cast<Impl*>(context);
    auto** event_paths = static_cast<char**>(paths);
    std::filesystem::path watched_root;
    {
      std::scoped_lock lock(self->mutex);
      watched_root = self->root;
    }
    std::vector<WorkspaceChange> changes;
    changes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const std::filesystem::path path(event_paths[index]);
      std::error_code error;
      const auto relative = std::filesystem::relative(path, watched_root, error);
      if (error || relative.empty() || ignored_relative(relative))
        continue;
      WorkspaceChangeKind kind = WorkspaceChangeKind::modified;
      if (flags[index] & (kFSEventStreamEventFlagItemCreated |
                          kFSEventStreamEventFlagItemRenamed))
        kind = WorkspaceChangeKind::created;
      if (flags[index] & kFSEventStreamEventFlagItemRemoved)
        kind = WorkspaceChangeKind::removed;
      changes.push_back({kind, path});
    }
    self->publish(std::move(changes), watched_root);
  }
#endif

  void run(std::stop_token stop) {
#if defined(_WIN32)
    std::array<std::byte, 64u * 1024u> buffer{};
    while (!stop.stop_requested()) {
      std::filesystem::path watched_root;
      {
        std::unique_lock lock(mutex);
        if (root.empty()) {
          wake.wait_for(lock, stop, std::chrono::milliseconds(200),
                        [this] { return !root.empty(); });
          continue;
        }
        watched_root = root;
      }
      const auto directory = CreateFileW(
          watched_root.c_str(), FILE_LIST_DIRECTORY,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
          nullptr);
      if (directory == INVALID_HANDLE_VALUE) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(300), [] { return false; });
        continue;
      }
      const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!event) {
        CloseHandle(directory);
        continue;
      }
      bool reopen = false;
      while (!stop.stop_requested() && !reopen) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        ResetEvent(event);
        const BOOL started = ReadDirectoryChangesW(
            directory, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION,
            nullptr, &overlapped, nullptr);
        if (!started)
          break;
        {
          std::scoped_lock lock(mutex);
          if (root == watched_root) {
            ready_root = watched_root;
            wake.notify_all();
          }
        }
        DWORD wait_result = WAIT_TIMEOUT;
        while (wait_result == WAIT_TIMEOUT && !stop.stop_requested()) {
          wait_result = WaitForSingleObject(event, 200);
          std::scoped_lock lock(mutex);
          reopen = root != watched_root;
          if (reopen)
            break;
        }
        if (stop.stop_requested() || reopen) {
          CancelIoEx(directory, &overlapped);
          break;
        }
        if (wait_result != WAIT_OBJECT_0)
          break;
        DWORD transferred = 0;
        if (!GetOverlappedResult(directory, &overlapped, &transferred, FALSE))
          break;
        std::vector<WorkspaceChange> changes;
        std::size_t offset = 0;
        while (offset < transferred) {
          const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
              buffer.data() + offset);
          const std::filesystem::path relative(std::wstring_view(
              info->FileName, info->FileNameLength / sizeof(wchar_t)));
          if (!relative.empty() && !ignored_relative(relative)) {
            WorkspaceChangeKind kind = WorkspaceChangeKind::modified;
            if (info->Action == FILE_ACTION_ADDED ||
                info->Action == FILE_ACTION_RENAMED_NEW_NAME)
              kind = WorkspaceChangeKind::created;
            else if (info->Action == FILE_ACTION_REMOVED ||
                     info->Action == FILE_ACTION_RENAMED_OLD_NAME)
              kind = WorkspaceChangeKind::removed;
            changes.push_back({kind, watched_root / relative});
          }
          if (info->NextEntryOffset == 0)
            break;
          offset += info->NextEntryOffset;
        }
        publish(std::move(changes), watched_root);
      }
      CloseHandle(event);
      CloseHandle(directory);
    }
#elif defined(__linux__)
    while (!stop.stop_requested()) {
      std::filesystem::path watched_root;
      {
        std::unique_lock lock(mutex);
        if (root.empty()) {
          wake.wait_for(lock, stop, std::chrono::milliseconds(200),
                        [this] { return !root.empty(); });
          continue;
        }
        watched_root = root;
      }
      const int descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (descriptor < 0)
        continue;
      std::unordered_map<int, std::filesystem::path> watches;
      const auto add_watch = [&](const std::filesystem::path& directory) {
        const int watch = inotify_add_watch(
            descriptor, directory.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY |
                IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB |
                IN_DELETE_SELF | IN_MOVE_SELF);
        if (watch >= 0)
          watches[watch] = directory;
      };
      add_watch(watched_root);
      std::error_code scan_error;
      for (std::filesystem::recursive_directory_iterator iterator(
               watched_root,
               std::filesystem::directory_options::skip_permission_denied,
               scan_error), end;
           !scan_error && iterator != end; iterator.increment(scan_error)) {
        if (ignored(iterator->path())) {
          if (iterator->is_directory(scan_error))
            iterator.disable_recursion_pending();
          scan_error.clear();
          continue;
        }
        if (iterator->is_directory(scan_error))
          add_watch(iterator->path());
        scan_error.clear();
      }
      {
        std::scoped_lock lock(mutex);
        if (root == watched_root) {
          ready_root = watched_root;
          wake.notify_all();
        }
      }
      std::array<std::byte, 64u * 1024u> buffer{};
      bool reopen = false;
      while (!stop.stop_requested() && !reopen) {
        pollfd item{descriptor, POLLIN, 0};
        const int ready = poll(&item, 1, 200);
        {
          std::scoped_lock lock(mutex);
          reopen = root != watched_root;
        }
        if (ready <= 0 || reopen)
          continue;
        const auto count = read(descriptor, buffer.data(), buffer.size());
        if (count <= 0)
          continue;
        std::vector<WorkspaceChange> changes;
        for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
          const auto* event = reinterpret_cast<const inotify_event*>(
              buffer.data() + offset);
          const auto watched = watches.find(event->wd);
          if (watched != watches.end()) {
            const auto path = event->len && event->name[0]
                ? watched->second / event->name : watched->second;
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(
                path, watched_root, relative_error);
            if (!relative_error && !ignored_relative(relative)) {
              WorkspaceChangeKind kind = WorkspaceChangeKind::modified;
              if (event->mask & (IN_CREATE | IN_MOVED_TO))
                kind = WorkspaceChangeKind::created;
              else if (event->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF))
                kind = WorkspaceChangeKind::removed;
              changes.push_back({kind, path});
              if ((event->mask & IN_ISDIR) &&
                  (event->mask & (IN_CREATE | IN_MOVED_TO)))
                add_watch(path);
            }
          }
          offset += sizeof(inotify_event) + event->len;
        }
        publish(std::move(changes), watched_root);
      }
      close(descriptor);
    }
#elif defined(__APPLE__)
    while (!stop.stop_requested()) {
      std::filesystem::path watched_root;
      {
        std::unique_lock lock(mutex);
        if (root.empty()) {
          wake.wait_for(lock, stop, std::chrono::milliseconds(200),
                        [this] { return !root.empty(); });
          continue;
        }
        watched_root = root;
      }
      const auto path_text = watched_root.string();
      auto* path = CFStringCreateWithFileSystemRepresentation(
          kCFAllocatorDefault, path_text.c_str());
      const void* values[] = {path};
      auto* paths = CFArrayCreate(kCFAllocatorDefault, values, 1,
                                  &kCFTypeArrayCallBacks);
      FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
      auto stream = FSEventStreamCreate(
          kCFAllocatorDefault, &Impl::fsevents_callback, &context, paths,
          kFSEventStreamEventIdSinceNow, 0.08,
          kFSEventStreamCreateFlagFileEvents |
              kFSEventStreamCreateFlagNoDefer);
      CFRelease(paths);
      CFRelease(path);
      if (!stream)
        continue;
      FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(),
                                       kCFRunLoopDefaultMode);
      FSEventStreamStart(stream);
      {
        std::scoped_lock lock(mutex);
        if (root == watched_root) {
          ready_root = watched_root;
          wake.notify_all();
        }
      }
      bool reopen = false;
      while (!stop.stop_requested() && !reopen) {
        (void)CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, true);
        std::scoped_lock lock(mutex);
        reopen = root != watched_root;
      }
      FSEventStreamStop(stream);
      FSEventStreamInvalidate(stream);
      FSEventStreamRelease(stream);
    }
#else
    while (!stop.stop_requested()) {
      std::filesystem::path watched_root;
      StampMap previous;
      {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::milliseconds(180), [] { return false; });
        if (stop.stop_requested())
          break;
        watched_root = root;
        previous = stamps;
      }
      auto current = collect_stamps(watched_root);
      {
        std::scoped_lock lock(mutex);
        if (root == watched_root) {
          ready_root = watched_root;
          wake.notify_all();
        }
      }
      std::vector<WorkspaceChange> changes;
      changes.reserve(16);
      for (const auto& [path, stamp] : current) {
        const auto old = previous.find(path);
        if (old == previous.end())
          changes.push_back({WorkspaceChangeKind::created, watched_root / path});
        else if (old->second != stamp)
          changes.push_back({WorkspaceChangeKind::modified, watched_root / path});
      }
      for (const auto& [path, stamp] : previous) {
        (void)stamp;
        if (!current.contains(path))
          changes.push_back({WorkspaceChangeKind::removed, watched_root / path});
      }
      {
        std::scoped_lock lock(mutex);
        if (watched_root == root)
          stamps = std::move(current);
      }
      publish(std::move(changes), watched_root);
    }
#endif
  }
};

WorkspaceWatcher::WorkspaceWatcher(std::filesystem::path root)
    : impl_(std::make_unique<Impl>()) {
  reset(std::move(root));
}

WorkspaceWatcher::~WorkspaceWatcher() {
  impl_->worker.request_stop();
  impl_->wake.notify_all();
}

void WorkspaceWatcher::reset(std::filesystem::path root) {
  std::error_code error;
  root = std::filesystem::weakly_canonical(std::move(root), error);
  std::unique_lock lock(impl_->mutex);
  impl_->root = error ? std::filesystem::path{} : std::move(root);
  impl_->ready_root.clear();
  impl_->stamps = collect_stamps(impl_->root);
  impl_->pending.clear();
  impl_->wake.notify_all();
  if (!impl_->root.empty()) {
    const auto expected = impl_->root;
    (void)impl_->wake.wait_for(lock, std::chrono::seconds(2), [&] {
      return impl_->ready_root == expected;
    });
  }
}

std::vector<WorkspaceChange> WorkspaceWatcher::take_changes() {
  std::scoped_lock lock(impl_->mutex);
  std::vector<WorkspaceChange> result;
  result.swap(impl_->pending);
  return result;
}

WorkspaceService::WorkspaceService(std::filesystem::path root) {
  std::string error;
  if (!root.empty())
    (void)set_root(std::move(root), error);
}

bool WorkspaceService::set_root(std::filesystem::path root, std::string& error) {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(std::move(root), ec);
  if (ec || !std::filesystem::is_directory(canonical, ec)) {
    error = "workspace is not a readable directory";
    return false;
  }
  root_ = std::move(canonical);
  return true;
}

const std::filesystem::path& WorkspaceService::root() const noexcept {
  return root_;
}

bool WorkspaceService::contains(const std::filesystem::path& path) const {
  if (root_.empty())
    return false;
  std::error_code error;
  const auto target = std::filesystem::weakly_canonical(path, error);
  if (error)
    return false;
  const auto relative = std::filesystem::relative(target, root_, error);
  if (error || relative.is_absolute())
    return false;
  const auto first = relative.begin();
  return first == relative.end() || *first != "..";
}

std::vector<WorkspaceEntry> WorkspaceService::enumerate(
    const std::size_t max_entries, const std::size_t max_depth) const {
  std::vector<WorkspaceEntry> result;
  if (root_.empty())
    return result;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator iterator(
      root_, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  const IgnoreRules ignore_rules(root_);
  for (; !ec && iterator != end && result.size() < max_entries;
       iterator.increment(ec)) {
    const auto& entry = *iterator;
    if (ignore_rules.ignored_path(entry.path())) {
      if (entry.is_directory(ec))
        iterator.disable_recursion_pending();
      continue;
    }
    const auto depth = static_cast<std::size_t>(iterator.depth());
    if (depth >= max_depth && entry.is_directory(ec))
      iterator.disable_recursion_pending();
    const auto relative = std::filesystem::relative(entry.path(), root_, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    result.push_back({entry.path(), utf8(relative),
                      utf8(entry.path().filename()), depth,
                      entry.is_directory(ec), depth < 2});
    ec.clear();
  }
  return result;
}

std::future<std::vector<WorkspaceEntry>> WorkspaceService::enumerate_async(
    const std::size_t max_entries, const std::size_t max_depth) const {
  const auto root = root_;
  return std::async(std::launch::async, [root, max_entries, max_depth] {
    return WorkspaceService(root).enumerate(max_entries, max_depth);
  });
}

std::vector<WorkspaceEntry> WorkspaceService::children(
    const std::filesystem::path& relative_directory,
    const std::size_t max_entries) const {
  std::vector<WorkspaceEntry> result;
  const auto directory = root_ / relative_directory;
  if (!contains(directory))
    return result;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::directory_iterator end;
  const IgnoreRules ignore_rules(root_);
  for (; !error && iterator != end && result.size() < max_entries;
       iterator.increment(error)) {
    const auto& entry = *iterator;
    if (ignore_rules.ignored_path(entry.path()))
      continue;
    const auto relative = std::filesystem::relative(entry.path(), root_, error);
    if (error) {
      error.clear();
      continue;
    }
    result.push_back({entry.path(), utf8(relative),
                      utf8(entry.path().filename()), 0,
                      entry.is_directory(error), false});
    error.clear();
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    if (left.directory != right.directory)
      return left.directory > right.directory;
    return ascii_lower(left.name) < ascii_lower(right.name);
  });
  return result;
}

std::vector<WorkspaceSearchResult> WorkspaceService::search(
    std::string query, const std::size_t max_results,
    const std::atomic_bool* cancelled) const {
  std::vector<WorkspaceSearchResult> result;
  query = ascii_lower(std::move(query));
  if (query.empty() || root_.empty())
    return result;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root_, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  const IgnoreRules ignore_rules(root_);
  for (; !error && iterator != end && result.size() < max_results;
       iterator.increment(error)) {
    if (cancelled && cancelled->load())
      break;
    const auto& entry = *iterator;
    if (ignore_rules.ignored_path(entry.path())) {
      if (entry.is_directory(error))
        iterator.disable_recursion_pending();
      error.clear();
      continue;
    }
    if (!entry.is_regular_file(error) || entry.file_size(error) > 2u * 1024u * 1024u) {
      error.clear();
      continue;
    }
    std::ifstream input(entry.path(), std::ios::binary);
    std::string line;
    std::size_t line_number = 0;
    while (input && std::getline(input, line) && result.size() < max_results) {
      ++line_number;
      if (line.find('\0') != std::string::npos)
        break;
      const auto lower = ascii_lower(line);
      std::size_t offset = 0;
      while ((offset = lower.find(query, offset)) != std::string::npos &&
             result.size() < max_results) {
        auto relative = std::filesystem::relative(entry.path(), root_, error);
        if (!error)
          result.push_back({utf8(relative), line_number, offset + 1, line});
        error.clear();
        offset += std::max<std::size_t>(query.size(), 1);
      }
    }
  }
  return result;
}

std::future<std::vector<WorkspaceSearchResult>> WorkspaceService::search_async(
    std::string query, const std::size_t max_results) const {
  const auto root = root_;
  return std::async(std::launch::async,
                    [root, query = std::move(query), max_results]() mutable {
                      return WorkspaceService(root).search(std::move(query),
                                                           max_results);
                    });
}

std::string WorkspaceService::read_text(const std::filesystem::path& path,
                                        const std::size_t max_bytes,
                                        std::string& error) const {
  if (!contains(path)) {
    error = "file is outside workspace";
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open file";
    return {};
  }
  std::string data;
  data.resize(max_bytes);
  input.read(data.data(), static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<std::size_t>(input.gcount()));
  if (data.find('\0') != std::string::npos) {
    error = "binary file preview is disabled";
    return {};
  }
  return data;
}

bool WorkspaceService::resolve_mutation_path(
    const std::filesystem::path& relative, const bool may_not_exist,
    std::filesystem::path& absolute, std::string& error) const {
  if (root_.empty() || relative.empty() || relative.is_absolute()) {
    error = "workspace mutation path must be relative";
    return false;
  }
  for (const auto& component : relative) {
    if (component == ".." || component == ".git" || component == ".tokmon") {
      error = "workspace mutation path is protected";
      return false;
    }
  }
  absolute = root_ / relative.lexically_normal();
  const auto containment_target = may_not_exist ? absolute.parent_path() : absolute;
  if (!contains(containment_target)) {
    error = "workspace mutation path escapes the workspace";
    return false;
  }
  return true;
}

bool WorkspaceService::create_file(const std::filesystem::path& relative,
                                   const std::string_view initial_text,
                                   std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_mutation_path(relative, true, absolute, error))
    return false;
  std::error_code filesystem_error;
  if (std::filesystem::exists(absolute, filesystem_error)) {
    error = "file already exists";
    return false;
  }
  if (!std::filesystem::is_directory(absolute.parent_path(), filesystem_error)) {
    error = "parent directory does not exist";
    return false;
  }
  std::ofstream output(absolute, std::ios::binary | std::ios::trunc);
  output.write(initial_text.data(), static_cast<std::streamsize>(initial_text.size()));
  output.flush();
  if (!output) {
    error = "cannot create file";
    return false;
  }
  return true;
}

bool WorkspaceService::create_directory(const std::filesystem::path& relative,
                                        std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_mutation_path(relative, true, absolute, error))
    return false;
  std::error_code filesystem_error;
  if (!std::filesystem::create_directory(absolute, filesystem_error)) {
    error = filesystem_error ? filesystem_error.message() : "directory already exists";
    return false;
  }
  return true;
}

bool WorkspaceService::rename_entry(const std::filesystem::path& relative,
                                    const std::filesystem::path& new_name,
                                    std::string& error) const {
  if (new_name.empty() || new_name.has_parent_path() || new_name == "." ||
      new_name == "..") {
    error = "new name must be one file name";
    return false;
  }
  std::filesystem::path source;
  if (!resolve_mutation_path(relative, false, source, error))
    return false;
  const auto target_relative = relative.parent_path() / new_name;
  std::filesystem::path target;
  if (!resolve_mutation_path(target_relative, true, target, error))
    return false;
  std::error_code filesystem_error;
  if (std::filesystem::exists(target, filesystem_error)) {
    error = "rename destination already exists";
    return false;
  }
  std::filesystem::rename(source, target, filesystem_error);
  if (filesystem_error) {
    error = "cannot rename workspace entry: " + filesystem_error.message();
    return false;
  }
  return true;
}

bool WorkspaceService::remove_entry(const std::filesystem::path& relative,
                                    const bool recursive,
                                    std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_mutation_path(relative, false, absolute, error))
    return false;
  std::error_code filesystem_error;
  if (!std::filesystem::exists(absolute, filesystem_error)) {
    error = "workspace entry does not exist";
    return false;
  }
  if (std::filesystem::is_directory(absolute, filesystem_error) && !recursive &&
      !std::filesystem::is_empty(absolute, filesystem_error)) {
    error = "directory is not empty";
    return false;
  }
  if (recursive)
    (void)std::filesystem::remove_all(absolute, filesystem_error);
  else
    (void)std::filesystem::remove(absolute, filesystem_error);
  if (filesystem_error) {
    error = "cannot remove workspace entry: " + filesystem_error.message();
    return false;
  }
  return true;
}

} // namespace tokmon::desk
