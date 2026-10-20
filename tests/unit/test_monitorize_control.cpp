/**
 * @file tests/unit/test_monitorize_control.cpp
 * @brief Exercise the private Monitorize display preparation protocol against real Unix sockets.
 */
#include "src/monitorize_control.h"
#include <gtest/gtest.h>
#ifdef __linux__
#include <future>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  /** @brief Serve one response over a private Unix socket and return the received JSON. */
  class control_peer {
  public:
    std::string path; ///< Temporary socket path.
    int listener; ///< Listening socket.
    std::future<nlohmann::json> request; ///< Received request.
    /** @brief Listen and reply with the supplied protocol response. @param reply Serialized response. */
    explicit control_peer(std::string reply, int delay_ms = 0) {
      char directory[] = "/tmp/monitorize-control-test-XXXXXX";
      path = std::string(mkdtemp(directory)) + "/control.sock";
      listener = socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::copy(path.begin(), path.end(), address.sun_path);
      bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
      listen(listener, 1);
      request = std::async(std::launch::async, [this, reply, delay_ms] {
        int peer = accept(listener, nullptr, nullptr);
        std::string line;
        char byte;
        while (recv(peer, &byte, 1, 0) == 1 && byte != '\n') { line += byte; }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        send(peer, reply.data(), reply.size(), MSG_NOSIGNAL);
        close(peer);
        return nlohmann::json::parse(line);
      });
    }
    /** @brief Remove the listener and its private directory. */
    ~control_peer() { close(listener); unlink(path.c_str()); rmdir(path.substr(0, path.rfind('/')).c_str()); }
  };

  TEST(MonitorizeControl, FingerprintsCertificateWithoutTrustingClientUniqueId) {
    EXPECT_EQ(monitorize_control::certificate_id("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(monitorize_control::certificate_id("").empty());
  }

  TEST(MonitorizeControl, SendsTypedAuthenticatedRequestAndParsesExactTarget) {
    control_peer peer(R"({"version":1,"ok":true,"target":{"capture":"pipewire_node","output_name":"42","pipewire_node":42,"width":2560,"height":1600,"fps":59.94,"offset_x":-100,"offset_y":0}})" "\n");
    monitorize_control::endpoint endpoint {peer.path, "private-token", 2};
    auto reply = monitorize_control::exchange(endpoint, "prepare", "certificate PEM", "launch-7", 2560, 1600, 59.94);
    ASSERT_TRUE(reply.ok) << reply.error;
    EXPECT_EQ(reply.target.output_name, "42");
    EXPECT_EQ(reply.target.offset_x, -100);
    EXPECT_DOUBLE_EQ(reply.target.fps, 59.94);
    auto request = peer.request.get();
    EXPECT_EQ(request["version"], 1);
    EXPECT_EQ(request["instance"], 2);
    EXPECT_EQ(request["token"], "private-token");
    EXPECT_EQ(request["client_id"], "certificate PEM");
    EXPECT_EQ(request["request_id"], "launch-7");
    EXPECT_EQ(request["width"], 2560);
    EXPECT_DOUBLE_EQ(request["fps"].get<double>(), 59.94);
  }

  TEST(MonitorizeControl, RejectsUnboundOrUnsupportedCaptureTargets) {
    for (const auto &target : {R"({"capture":"kms","output_name":"","width":1920,"height":1080,"fps":60})", R"({"capture":"auto","output_name":"DP-1","width":1920,"height":1080,"fps":60})", R"({"capture":"pipewire_node","output_name":"42","width":1920,"height":1080,"fps":60})"}) {
      control_peer peer(std::string("{\"version\":1,\"ok\":true,\"target\":") + target + "}\n");
      EXPECT_FALSE(monitorize_control::exchange({peer.path, "token", 1}, "prepare", "cert", "1", 1920, 1080, 60).ok);
      peer.request.get();
    }
  }

  TEST(MonitorizeControl, RejectsWrongVersionOversizedAndMalformedReplies) {
    for (const auto &response : {std::string("{\"version\":2,\"ok\":true}\n"), std::string("not-json\n"), std::string(65537, 'x') + "\n"}) {
      control_peer peer(response);
      EXPECT_FALSE(monitorize_control::exchange({peer.path, "token", 1}, "prepare", "cert", "1", 1920, 1080, 60).ok);
      peer.request.get();
    }
  }

  TEST(MonitorizeControl, PreservesHostRejectionAndAcknowledgesLifecycle) {
    control_peer peer("{\"version\":1,\"ok\":false,\"error\":\"Slot busy\"}\n");
    auto reply = monitorize_control::exchange({peer.path, "token", 1}, "prepare", "cert", "1", 1920, 1080, 60);
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(reply.error, "Slot busy");
    peer.request.get();
    control_peer lifecycle("{\"version\":1,\"ok\":true}\n");
    EXPECT_TRUE(monitorize_control::exchange({lifecycle.path, "token", 1}, "stream_stopped", "cert", "1").ok);
    EXPECT_EQ(lifecycle.request.get()["operation"], "stream_stopped");
  }

  TEST(MonitorizeControl, BoundsResponseDeadlineAndRejectsClosedPeer) {
    control_peer delayed("{\"version\":1,\"ok\":true}\n", 80);
    auto start = std::chrono::steady_clock::now();
    auto response = monitorize_control::exchange({delayed.path, "token", 1}, "release", "cert", "1", 0, 0, 0, 15);
    EXPECT_FALSE(response.ok);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(70));
    delayed.request.get();
    control_peer closed("");
    EXPECT_FALSE(monitorize_control::exchange({closed.path, "token", 1}, "release", "cert", "1").ok);
    closed.request.get();
  }

  TEST(MonitorizeControl, CancelsSocketWaitWithoutWaitingForPrepareDeadline) {
    control_peer delayed("{\"version\":1,\"ok\":true}\n", 250);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto cancellation = std::async(std::launch::async, [cancelled] {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      cancelled->store(true);
    });
    auto start = std::chrono::steady_clock::now();
    auto response = monitorize_control::exchange({delayed.path, "token", 1}, "release", "cert", "1", 0, 0, 0, 20000, cancelled);
    EXPECT_FALSE(response.ok);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(220));
    cancellation.get();
    delayed.request.get();
  }

  TEST(MonitorizeControl, SerializesLaunchAdmissionAndOwnerCancellation) {
    monitorize_control::operation_gate gate;
    auto active = gate.begin("client A");
    ASSERT_TRUE(active);
    EXPECT_FALSE(gate.begin("client B"));
    EXPECT_FALSE(gate.cancel("client B"));
    EXPECT_FALSE(active->load());
    EXPECT_TRUE(gate.cancel("client A"));
    EXPECT_TRUE(active->load());
    gate.finish_launch();
    EXPECT_FALSE(gate.begin("client A"));
    gate.finish_cancel();
    EXPECT_TRUE(gate.begin("client B"));
    gate.shutdown();
    gate.finish_launch();
    EXPECT_FALSE(gate.begin("client A"));
  }

  TEST(MonitorizeControl, KeepsVerifiedIdentityBoundToEachLiveConnection) {
    monitorize_control::certificate_registry registry;
    auto first = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);
    registry.record("127.0.0.1:1001", first, "certificate A");
    registry.record("127.0.0.1:1002", second, "certificate B");
    EXPECT_EQ(registry.lookup("127.0.0.1:1001"), "certificate A");
    EXPECT_EQ(registry.lookup("127.0.0.1:1002"), "certificate B");
    first.reset();
    EXPECT_TRUE(registry.lookup("127.0.0.1:1001").empty());
    registry.record("127.0.0.1:1001", second, "certificate C");
    EXPECT_EQ(registry.lookup("127.0.0.1:1001"), "certificate C");
    EXPECT_TRUE(registry.lookup("unknown").empty());
  }

  TEST(MonitorizeControl, ParsesFractionalClientModesAndRejectsTrailingJunk) {
    int width = 0, height = 0;
    double fps = 0;
    EXPECT_TRUE(monitorize_control::parse_mode("2560x1600x59.94", width, height, fps));
    EXPECT_EQ(width, 2560);
    EXPECT_EQ(height, 1600);
    EXPECT_DOUBLE_EQ(fps, 59.94);
    for (const auto &mode : {"1920x1080x60junk", "1920x1080x60x120", "0x0x0", "1920.5x1080x60", "1920x1080xnan", "1920x1080x1001"}) {
      EXPECT_FALSE(monitorize_control::parse_mode(mode, width, height, fps));
    }
  }

  TEST(MonitorizeControl, PublishesImmutableCaptureSnapshot) {
    monitorize_control::capture_target target;
    target.output_name = "42";
    target.width = 1920;
    monitorize_control::bind_target(target);
    auto original = monitorize_control::current_target();
    ASSERT_TRUE(original);
    target.width = 2560;
    monitorize_control::bind_target(target);
    EXPECT_EQ(original->width, 1920);
    EXPECT_EQ(monitorize_control::current_target()->width, 2560);
    monitorize_control::clear_target();
    EXPECT_FALSE(monitorize_control::current_target());
    EXPECT_EQ(original->width, 1920);
  }

  TEST(MonitorizeControl, RejectsFractionalPixelDimensions) {
    control_peer peer(R"({"version":1,"ok":true,"target":{"capture":"kms","output_name":"1","width":1920.5,"height":1080,"fps":60}})" "\n");
    EXPECT_FALSE(monitorize_control::exchange({peer.path, "token", 1}, "prepare", "cert", "1", 1920, 1080, 60).ok);
    peer.request.get();
  }

  TEST(MonitorizeControl, RefusesMissingAuthenticationAndInvalidModes) {
    EXPECT_FALSE(monitorize_control::exchange({"/tmp/missing.sock", "", 1}, "prepare", "cert", "1", 1920, 1080, 60).ok);
    EXPECT_FALSE(monitorize_control::exchange({"/tmp/missing.sock", "token", 1}, "prepare", "", "1", 1920, 1080, 60).ok);
    EXPECT_FALSE(monitorize_control::exchange({"/tmp/missing.sock", "token", 1}, "prepare", "cert", "1", 0, 1080, 60).ok);
  }

  TEST(MonitorizeControl, ReportsGeometryRevalidationAfterUsableFrame) {
    control_peer peer(R"({"version":1,"ok":true,"layout_changed":true})" "\n");
    auto result = monitorize_control::exchange({peer.path, "token", 1}, "stream_ready", "cert", "1", 0, 0, 0);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.layout_changed);
    EXPECT_EQ(peer.request.get()["operation"], "stream_ready");
  }
}
#endif

