/**
 * @file src/monitorize_control.h
 * @brief Bounded, authenticated local control channel for client-requested Linux displays.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#ifdef __linux__
  #include <cerrno>
  #include <chrono>
  #include <cstring>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace monitorize_control {
  inline constexpr char protocol_marker[] = "MONITORIZE_CONTROL_PROTOCOL=1"; ///< Binary capability marker inspected before managed launch.
  inline constexpr char layout_protocol_marker[] = "MONITORIZE_LAYOUT_RECOVERY=1"; ///< First-encoded-frame lifecycle support for guarded layouts.
  inline constexpr size_t maximum_message = 65536; ///< Maximum newline-delimited JSON frame size.

  /**
   * @brief Derive a stable bounded identity from a verified certificate.
   * @param certificate Verified PEM certificate retained by the TLS authorization callback.
   * @return SHA256 hexadecimal fingerprint, or empty when no identity was verified.
   */
  inline std::string certificate_id(const std::string &certificate) {
    if (certificate.empty()) {
      return {};
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(certificate.data()), certificate.size(), digest);
    constexpr char hex[] = "0123456789abcdef";
    std::string fingerprint;
    fingerprint.reserve(SHA256_DIGEST_LENGTH * 2);
    for (auto byte : digest) {
      fingerprint += hex[byte >> 4];
      fingerprint += hex[byte & 15];
    }
    return fingerprint;
  }

  /** @brief Verified identities associated with live TLS connections rather than handshake order. */
  class certificate_registry {
  public:
    /**
     * @brief Remember the verified certificate for a particular accepted connection.
     * @param key Unique local-server remote endpoint tuple.
     * @param connection Lifetime token for that accepted connection.
     * @param certificate Verified PEM certificate.
     */
    void record(const std::string &key, std::shared_ptr<void> connection, std::string certificate) {
      std::lock_guard lock(mutex);
      for (auto entry = identities.begin(); entry != identities.end();) {
        if (entry->second.first.expired()) {
          entry = identities.erase(entry);
        } else {
          ++entry;
        }
      }
      identities[key] = {connection, std::move(certificate)};
    }

    /** @brief Get the certificate for this request's still-live connection. @param key Remote endpoint tuple. @return Verified PEM or empty when unavailable. */
    std::string lookup(const std::string &key) {
      std::lock_guard lock(mutex);
      auto entry = identities.find(key);
      if (entry == identities.end() || entry->second.first.expired()) {
        return {};
      }
      return entry->second.second;
    }

  private:
    std::mutex mutex; ///< Serializes verification and request lookup.
    std::map<std::string, std::pair<std::weak_ptr<void>, std::string>> identities; ///< Weak connection ownership prevents stale endpoint reuse.
  };

  /** @brief Bound admission and cancellation for one serialized managed worker. */
  class operation_gate {
  public:
    /** @brief Reserve a launch transaction. @param client Verified owner. @return Cancellation flag, or null if busy/stopping. */
    std::shared_ptr<std::atomic_bool> begin(const std::string &client) {
      std::lock_guard lock(mutex);
      if (stopping || busy || client.empty()) {
        return {};
      }
      busy = true;
      owner = client;
      cancellation = std::make_shared<std::atomic_bool>(false);
      return cancellation;
    }

    /** @brief Queue an owner-authorized cancellation without blocking the HTTPS loop. @param client Verified owner. @return Whether one cancellation was admitted. */
    bool cancel(const std::string &client) {
      std::lock_guard lock(mutex);
      if (stopping || cancel_queued || client.empty() || (busy && owner != client)) {
        return false;
      }
      busy = true;
      owner = client;
      cancel_queued = true;
      if (cancellation) {
        cancellation->store(true);
      }
      return true;
    }

    /** @brief Release launch admission, retaining it while an explicit cancellation is queued. */
    void finish_launch() {
      std::lock_guard lock(mutex);
      if (!cancel_queued) {
        busy = false;
        owner.clear();
        cancellation.reset();
      }
    }

    /** @brief Release the cancellation and allow the next launch after cleanup finishes. */
    void finish_cancel() {
      std::lock_guard lock(mutex);
      cancel_queued = false;
      busy = false;
      owner.clear();
      cancellation.reset();
    }

    /** @brief Cancel preparation and permanently close admission during host shutdown. */
    void shutdown() {
      std::lock_guard lock(mutex);
      stopping = true;
      if (cancellation) {
        cancellation->store(true);
      }
    }

  private:
    std::mutex mutex; ///< Serializes HTTPS admission and worker completion.
    bool busy = false; ///< At most one accepted launch/cancel sequence.
    bool cancel_queued = false; ///< At most one explicit cancellation waits for the worker.
    bool stopping = false; ///< Host shutdown rejects all further work.
    std::string owner; ///< Verified owner of any active preparation.
    std::shared_ptr<std::atomic_bool> cancellation; ///< Cooperative cancellation flag for the current preparation.
  };

  /** @brief Private endpoint inherited from Monitorize, never supplied by a network client. */
  struct endpoint {
    std::string socket; ///< Unix socket path in the user's private runtime directory.
    std::string token; ///< Per-process authentication credential.
    int instance; ///< Configured display slot (1 or 2).
  };

  /** @brief Exact capture target returned after successful display preparation. */
  struct capture_target {
    std::string output_name; ///< Backend-specific exact output identifier.
    std::string capture; ///< Explicit capture backend; automatic selection is forbidden.
    int pipewire_node = 0; ///< Optional PipeWire node ID.
    int width = 0; ///< Actual display width.
    int height = 0; ///< Actual display height.
    int offset_x = 0; ///< Desktop X coordinate, including negative positions.
    int offset_y = 0; ///< Desktop Y coordinate.
    double fps = 0; ///< Actual display refresh rate.
  };

  inline std::atomic<std::shared_ptr<const capture_target>> bound_target; ///< Atomically published capture metadata; readers retain immutable snapshots.
  inline std::atomic_uint64_t layout_generation {0}; ///< Capture workers rebuild geometry after a guarded layout transaction.

  /** @brief Publish target metadata without changing environment variables. @param target Verified capture target. */
  inline void bind_target(const capture_target &target) {
    bound_target.store(std::make_shared<const capture_target>(target));
  }

  /** @brief Remove a terminated lease's target while retained worker snapshots stay valid. */
  inline void clear_target() {
    bound_target.store({});
  }

  /** @brief Retain the current immutable target for a capture worker. @return Target snapshot, or null before preparation. */
  inline std::shared_ptr<const capture_target> current_target() {
    return bound_target.load();
  }

  /** @brief Result of a local control operation. */
  struct result {
    bool ok = false; ///< Whether Monitorize accepted and completed the operation.
    bool layout_changed = false; ///< Revalidate capture geometry after a completed layout change.
    std::string error; ///< Safe diagnostic suitable for the requesting client.
    capture_target target; ///< Valid only after a successful prepare operation.
  };

  /** @brief Read a control setting without modifying the process environment. @param name Environment key. @return Value or empty string. */
  inline std::string environment(const char *name) {
    const auto value = std::getenv(name);
    return value ? value : "";
  }

  /** @brief Detect managed listeners, including misconfigured ones which must fail closed. @return Whether a control socket was configured. */
  inline bool lifecycle_enabled() {
#ifdef __linux__
    return !environment("MONITORIZE_CONTROL_SOCKET").empty();
#else
    return false;
#endif
  }

  /** @brief Whether Monitorize creates the target at launch rather than holding a fixed display. */
  inline bool enabled() {
    return lifecycle_enabled() && environment("MONITORIZE_LIFECYCLE_ONLY") != "1";
  }

  /** @brief Read the private endpoint passed by Monitorize. @return Endpoint, with instance zero when invalid. */
  inline endpoint configured_endpoint() {
    auto instance = environment("MONITORIZE_CONTROL_INSTANCE");
    return {environment("MONITORIZE_CONTROL_SOCKET"), environment("MONITORIZE_CONTROL_TOKEN"), instance == "1" ? 1 : instance == "2" ? 2 : 0};
  }

  /** @brief Validate bounded positive client display settings. @param width Width. @param height Height. @param fps Requested frame rate. @return Whether the mode is structurally valid. */
  inline bool valid_mode(int width, int height, double fps) {
    return width > 0 && width <= 16384 && height > 0 && height <= 16384 && std::isfinite(fps) && fps > 0 && fps <= 1000;
  }

  /**
   * @brief Parse the GameStream mode without truncating fractional display refresh requests.
   * @param mode Client-selected WIDTHxHEIGHTxFPS string.
   * @param width Parsed width.
   * @param height Parsed height.
   * @param fps Parsed frame rate.
   * @return Whether every component is complete and within protocol bounds.
   */
  inline bool parse_mode(const std::string &mode, int &width, int &height, double &fps) {
    try {
      auto first = mode.find('x');
      auto second = first == std::string::npos ? first : mode.find('x', first + 1);
      if (first == std::string::npos || second == std::string::npos || mode.find('x', second + 1) != std::string::npos) {
        return false;
      }
      auto w = mode.substr(0, first);
      auto h = mode.substr(first + 1, second - first - 1);
      auto rate = mode.substr(second + 1);
      if (w.empty() || h.empty() || rate.empty() || w.find_first_not_of("0123456789") != std::string::npos || h.find_first_not_of("0123456789") != std::string::npos || rate.find_first_not_of("0123456789.") != std::string::npos) {
        return false;
      }
      size_t consumed;
      width = std::stoi(w);
      height = std::stoi(h);
      fps = std::stod(rate, &consumed);
      return consumed == rate.size() && valid_mode(width, height, fps);
    } catch (...) {
      return false;
    }
  }

  /**
   * @brief Exchange one JSON operation with Monitorize using an absolute bounded deadline.
   * @param destination Private local endpoint.
   * @param operation prepare, abort, release, stream_started, stream_ready, or stream_stopped.
   * @param client_id Verified TLS certificate, never the unauthenticated uniqueid query argument.
   * @param request_id Launch generation used to reject stale lifecycle callbacks.
   * @param width Requested width for prepare.
   * @param height Requested height for prepare.
   * @param fps Requested frame rate for prepare.
   * @param timeout_ms Absolute timeout, capped at twenty seconds for preparation and thirty for layout recovery.
   * @param cancelled Optional transaction cancellation flag checked while waiting.
   * @return Validated response or a failure; no capture fallback is permitted.
   */
  inline result exchange(const endpoint &destination, const std::string &operation, const std::string &client_id, const std::string &request_id, int width = 0, int height = 0, double fps = 0, int timeout_ms = 20000, std::shared_ptr<const std::atomic_bool> cancelled = {}) {
    result response;
    if (destination.socket.empty() || destination.token.empty() || destination.instance < 1 || destination.instance > 2 || client_id.empty() || request_id.empty()) {
      response.error = "Missing Monitorize control authentication";
      return response;
    }
    if (operation != "prepare" && operation != "release" && operation != "abort" && operation != "stream_started" && operation != "stream_ready" && operation != "stream_stopped") {
      response.error = "Unsupported Monitorize control operation";
      return response;
    }
    if (operation == "prepare" && !valid_mode(width, height, fps)) {
      response.error = "Invalid client display settings";
      return response;
    }
#ifdef __linux__
    int fd = -1;
    try {
      nlohmann::json message {{"version", 1}, {"token", destination.token}, {"instance", destination.instance}, {"operation", operation}, {"client_id", client_id}, {"request_id", request_id}};
      if (operation == "prepare") {
        message.update({{"width", width}, {"height", height}, {"fps", fps}});
      }
      auto payload = message.dump() + "\n";
      if (payload.size() > maximum_message || destination.socket.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Monitorize control request exceeds size limit");
      }
      fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (fd < 0) {
        throw std::runtime_error("Cannot create Monitorize control socket");
      }
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::clamp(timeout_ms, 1, operation == "prepare" ? 20000 : 30000));
      auto wait = [&](short events) {
        while (true) {
          if (cancelled && cancelled->load()) {
            throw std::runtime_error("Monitorize launch was cancelled");
          }
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
          if (remaining <= 0) {
            throw std::runtime_error("Monitorize control request timed out");
          }
          pollfd descriptor {fd, events, 0};
          int ready = poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining, 100)));
          if (ready > 0) {
            if (cancelled && cancelled->load()) {
              throw std::runtime_error("Monitorize launch was cancelled");
            }
            return;
          }
          if (ready == 0) {
            continue;
          }
          if (errno != EINTR) {
            throw std::runtime_error("Monitorize control request timed out");
          }
        }
      };
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, destination.socket.c_str(), destination.socket.size() + 1);
      if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
          throw std::runtime_error("Monitorize control service unavailable");
        }
        wait(POLLOUT);
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
          throw std::runtime_error("Monitorize control connection failed");
        }
      }
      ucred credentials {};
      socklen_t credential_size = sizeof(credentials);
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) < 0 || credentials.uid != getuid()) {
        throw std::runtime_error("Monitorize control service has an unexpected owner");
      }
      size_t sent = 0;
      while (sent < payload.size()) {
        wait(POLLOUT);
        auto count = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
          continue;
        }
        if (count <= 0) {
          throw std::runtime_error("Monitorize control request failed");
        }
        sent += count;
      }
      std::string line;
      while (line.find('\n') == std::string::npos) {
        wait(POLLIN);
        char buffer[4096];
        auto count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
          continue;
        }
        if (count <= 0) {
          throw std::runtime_error("Monitorize closed the control connection");
        }
        line.append(buffer, count);
        if (line.size() > maximum_message) {
          throw std::runtime_error("Monitorize control response exceeds size limit");
        }
      }
      close(fd);
      fd = -1;
      auto reply = nlohmann::json::parse(line.substr(0, line.find('\n')));
      response.layout_changed = reply.value("layout_changed", false);
      if (!reply.at("version").is_number_integer() || reply.at("version") != 1 || !reply.at("ok").is_boolean()) {
        throw std::runtime_error("Unsupported Monitorize control response");
      }
      if (!reply.at("ok").get<bool>()) {
        response.error = reply.value("error", std::string("Monitorize rejected the request"));
        return response;
      }
      if (operation == "prepare") {
        const auto &target = reply.at("target");
        auto &value = response.target;
        for (const char *key : {"width", "height"}) {
          if (!target.at(key).is_number_integer()) {
            throw std::runtime_error("Monitorize returned non-integer pixel dimensions");
          }
        }

        value.output_name = target.at("output_name").get<std::string>();
        value.capture = target.at("capture").get<std::string>();
        value.width = target.at("width").get<int>();
        value.height = target.at("height").get<int>();
        value.fps = target.at("fps").get<double>();
        value.offset_x = target.value("offset_x", 0);
        value.offset_y = target.value("offset_y", 0);
        value.pipewire_node = target.value("pipewire_node", 0);
        bool explicit_backend = value.capture == "kms" || value.capture == "wlr" || value.capture == "kwin" || value.capture == "x11" || value.capture == "nvfbc" || value.capture == "pipewire_node";
        if (value.output_name.empty() || !explicit_backend || !valid_mode(value.width, value.height, value.fps) || (value.capture == "pipewire_node" && value.pipewire_node <= 0)) {
          throw std::runtime_error("Monitorize did not return an exact capture target");
        }
      }
      response.ok = true;
    } catch (const std::exception &error) {
      if (fd >= 0) {
        close(fd);
      }
      // Never propagate parser exceptions containing secret tokens or certificate payloads.
      response.error = dynamic_cast<const nlohmann::json::exception *>(&error) ? "Invalid Monitorize control response" : error.what();
    }
#else
    response.error = "Client-requested displays require Linux";
#endif
    return response;
  }
}
