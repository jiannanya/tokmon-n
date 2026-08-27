#include "tokmon/light_path.hpp"

#include "tokmon/ids.hpp"

namespace tokmon {

Result<std::shared_ptr<BeamRegistry::Ticket>> BeamRegistry::acquire(
    const LensId& lens, const GenerationId generation,
    RayId ray, const std::chrono::milliseconds timeout) {
  std::scoped_lock lock(mutex_);
  if (closed_generations_.contains({lens, generation}))
    return tl::unexpected(make_error(ErrorCode::invalid_state,
        "Lens generation is draining or closed: " + lens + "@" +
            std::to_string(generation)));
  auto ticket = std::make_shared<Ticket>();
  ticket->id = make_id("beam");
  ticket->lens = lens;
  ticket->generation = generation;
  ticket->ray = std::move(ray);
  ticket->deadline = std::chrono::steady_clock::now() + timeout;
  tickets_.emplace(ticket->id, ticket);
  return ticket;
}

std::size_t BeamRegistry::stop_ray(const RayId& ray) {
  std::scoped_lock lock(mutex_);
  std::size_t stopped = 0;
  for (const auto& [_, ticket] : tickets_)
    if (ticket->ray == ray) {
      ticket->stop.request_stop();
      ++stopped;
    }
  return stopped;
}

void BeamRegistry::release(const std::string& ticket_id) {
  std::scoped_lock lock(mutex_);
  tickets_.erase(ticket_id);
}

std::size_t BeamRegistry::stop_generation(const LensId& lens,
                                           const GenerationId generation) {
  std::scoped_lock lock(mutex_);
  closed_generations_.emplace(lens, generation);
  std::size_t stopped = 0;
  for (const auto& [_, ticket] : tickets_) {
    if (ticket->lens == lens && ticket->generation == generation) {
      ticket->stop.request_stop();
      ++stopped;
    }
  }
  return stopped;
}

void BeamRegistry::reopen_generation(const LensId& lens,
                                     const GenerationId generation) {
  std::scoped_lock lock(mutex_);
  closed_generations_.erase({lens, generation});
}

std::size_t BeamRegistry::active(const LensId& lens,
                                 const GenerationId generation) const {
  std::scoped_lock lock(mutex_);
  std::size_t count = 0;
  for (const auto& [_, ticket] : tickets_)
    if (ticket->lens == lens && ticket->generation == generation) ++count;
  return count;
}

}  // namespace tokmon
