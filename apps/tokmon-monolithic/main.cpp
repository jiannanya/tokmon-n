#include <string_view>

#include "apps/entrypoints.hpp"

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view role = argv[1];
    if (role == "--tokmon-internal-daemon")
      return tokmon::app::daemon_main(argc - 1, argv + 1);
    if (role == "--tokmon-internal-worker")
      return tokmon::app::lens_worker_main(argc - 1, argv + 1);
  }
  return tokmon::app::cli_main(argc, argv);
}
