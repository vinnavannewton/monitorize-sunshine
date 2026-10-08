/**
 * @file src/pairing_state.h
 * @brief Synchronized pairing results and lock-owned pending-session storage.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace pairing {
  /**
   * @brief Final handshake outcome shared with the PIN HTTP request.
   */
  struct completion_t {
    std::mutex mutex;  ///< Protects the result.
    std::condition_variable condition;  ///< Wakes the PIN request.
    std::optional<bool> result;  ///< Unset until verification or cancellation.

    /**
     * @brief Publish the first final outcome and wake waiters.
     * @param success Whether Moonlight completed verification.
     */
    void finish(bool success) {
      std::lock_guard lock {mutex};
      if (!result) {
        result = success;
        condition.notify_all();
      }
    }

    /**
     * @brief Wait until verification finishes or the deadline expires.
     * @param deadline Maximum wait time.
     * @return Verified outcome, or no value on timeout.
     */
    std::optional<bool> wait(std::chrono::steady_clock::time_point deadline) {
      std::unique_lock lock {mutex};
      condition.wait_until(lock, deadline, [this] { return result.has_value(); });
      return result;
    }
  };

  /**
   * @brief Lifecycle fields independent of cryptographic handshake state.
   */
  struct state_t {
    std::shared_ptr<completion_t> completion = std::make_shared<completion_t>();  ///< Shared final result.
    std::chrono::steady_clock::time_point expires_at = std::chrono::steady_clock::now() + std::chrono::minutes {2};  ///< Stale request deadline.
    bool submitted = false;  ///< PIN already supplied to this request.
    bool failed = false;  ///< Caller must erase after finishing its response.

    /**
     * @brief Fail without invalidating the caller's session reference.
     */
    void fail() {
      failed = true;
      completion->finish(false);
    }
  };

  /**
   * @brief Pairing storage whose unlocked operations require the caller's lock.
   * @tparam Session Session type inheriting state_t.
   */
  template<class Session>
  struct sessions_t {
    std::mutex mutex;  ///< Serializes all map access and protocol transitions.
    std::unordered_map<std::string, Session> entries;  ///< Live requests keyed by client ID.

    /**
     * @brief Replace a retry's old state, notifying its existing PIN waiter.
     * @param id Client identifier.
     * @param session Fresh request state.
     * @return Iterator to the new session.
     */
    auto insert_unlocked(const std::string &id, Session session) {
      if (auto it = entries.find(id); it != entries.end()) {
        it->second.completion->finish(false);
        entries.erase(it);
      }
      return entries.emplace(id, std::move(session)).first;
    }

    /**
     * @brief Cancel and remove expired sessions while holding the store lock.
     * @param now Current monotonic time.
     */
    void expire_unlocked(std::chrono::steady_clock::time_point now) {
      for (auto it = entries.begin(); it != entries.end();) {
        if (it->second.expires_at <= now) {
          it->second.completion->finish(false);
          it = entries.erase(it);
        } else {
          ++it;
        }
      }
    }

    /**
     * @brief Find a single request waiting for a PIN without choosing arbitrarily.
     * @return Iterator to the sole pending session, or end if absent/ambiguous.
     */
    auto pending_unlocked() {
      auto selected = entries.end();
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->second.submitted && !it->second.failed) {
          if (selected != entries.end()) {
            return entries.end();
          }
          selected = it;
        }
      }
      return selected;
    }
  };
}  // namespace pairing
