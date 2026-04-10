#pragma once

#include "volt/event/Event.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace volt::event {

class EventDispatcher {
 public:
  using ListenerId = std::uint64_t;
  using Listener = std::function<void(const Event&)>;

  EventDispatcher() = default;
  ~EventDispatcher() = default;

  EventDispatcher(const EventDispatcher&) = delete;
  EventDispatcher& operator=(const EventDispatcher&) = delete;

  ListenerId subscribe(Listener listener);
  ListenerId subscribe(EventType type, Listener listener);
  void unsubscribe(ListenerId listenerId);

  void enqueue(Event event);
  std::size_t dispatchQueued();

  [[nodiscard]] std::size_t pendingCount() const;

 private:
  struct ListenerEntry {
    ListenerId id{0};
    std::optional<EventType> typeFilter;
    Listener callback;
    bool active{true};
  };

  std::uint64_t nextListenerId_{1};
  std::uint64_t nextSequence_{1};
  std::vector<ListenerEntry> listeners_;
  std::deque<Event> queue_;
};

}  // namespace volt::event
