#include "volt/event/EventDispatcher.hpp"

#include <algorithm>
#include <utility>

namespace volt::event {

EventDispatcher::ListenerId EventDispatcher::subscribe(Listener listener) {
  const ListenerId listenerId = nextListenerId_++;
  listeners_.push_back(ListenerEntry{
      .id = listenerId,
      .typeFilter = std::nullopt,
      .callback = std::move(listener),
      .active = true,
  });
  return listenerId;
}

EventDispatcher::ListenerId EventDispatcher::subscribe(EventType type, Listener listener) {
  const ListenerId listenerId = nextListenerId_++;
  listeners_.push_back(ListenerEntry{
      .id = listenerId,
      .typeFilter = type,
      .callback = std::move(listener),
      .active = true,
  });
  return listenerId;
}

void EventDispatcher::unsubscribe(ListenerId listenerId) {
  for (ListenerEntry& listener : listeners_) {
    if (listener.id == listenerId) {
      listener.active = false;
      break;
    }
  }
}

void EventDispatcher::enqueue(Event event) {
  event.sequence = nextSequence_++;
  queue_.push_back(std::move(event));
}

std::size_t EventDispatcher::dispatchQueued() {
  const std::size_t initialEventCount = queue_.size();

  for (std::size_t i = 0; i < initialEventCount; ++i) {
    Event event = std::move(queue_.front());
    queue_.pop_front();

    for (const ListenerEntry& listener : listeners_) {
      if (!listener.active) {
        continue;
      }

      if (listener.typeFilter.has_value() && listener.typeFilter.value() != event.type) {
        continue;
      }

      listener.callback(event);
    }
  }

  listeners_.erase(
      std::remove_if(
          listeners_.begin(),
          listeners_.end(),
          [](const ListenerEntry& listener) { return !listener.active; }),
      listeners_.end());

  return initialEventCount;
}

std::size_t EventDispatcher::pendingCount() const {
  return queue_.size();
}

}  // namespace volt::event
