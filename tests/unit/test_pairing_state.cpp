/**
 * @file tests/unit/test_pairing_state.cpp
 * @brief Regression coverage for pairing retry ownership and final-result waits.
 */
#include <future>
#include <thread>
#include <gtest/gtest.h>
#include "src/pairing_state.h"

using namespace std::chrono_literals;

TEST(PairingState, FailureKeepsCallerReferenceAlive) {
  pairing::sessions_t<pairing::state_t> sessions;
  std::lock_guard lock {sessions.mutex};
  auto it = sessions.insert_unlocked("client", {});
  auto &session = it->second;
  session.fail();
  EXPECT_TRUE(session.failed);
  EXPECT_EQ(session.completion->wait(std::chrono::steady_clock::now()), false);
  EXPECT_EQ(sessions.entries.size(), 1);
  sessions.entries.erase(it);
}

TEST(PairingState, RetryReplacesOldPhaseAndCancelsOldWaiter) {
  pairing::sessions_t<pairing::state_t> sessions;
  std::lock_guard lock {sessions.mutex};
  auto old = sessions.insert_unlocked("client", {});
  old->second.submitted = true;
  auto completion = old->second.completion;
  auto replacement = sessions.insert_unlocked("client", {});
  EXPECT_EQ(completion->wait(std::chrono::steady_clock::now()), false);
  EXPECT_NE(completion, replacement->second.completion);
  EXPECT_FALSE(replacement->second.submitted);
  EXPECT_EQ(sessions.pending_unlocked(), replacement);
}

TEST(PairingState, RefusesAmbiguousRequestsAndIgnoresSubmittedOnes) {
  pairing::sessions_t<pairing::state_t> sessions;
  std::lock_guard lock {sessions.mutex};
  EXPECT_EQ(sessions.pending_unlocked(), sessions.entries.end());
  sessions.insert_unlocked("first", {});
  sessions.insert_unlocked("second", {});
  EXPECT_EQ(sessions.pending_unlocked(), sessions.entries.end());
  sessions.entries.at("first").submitted = true;
  EXPECT_EQ(sessions.pending_unlocked()->first, "second");
  sessions.entries.at("second").fail();
  EXPECT_EQ(sessions.pending_unlocked(), sessions.entries.end());
}

TEST(PairingState, ExpiryCancelsWaitAndPreservesLiveRequest) {
  pairing::sessions_t<pairing::state_t> sessions;
  std::lock_guard lock {sessions.mutex};
  auto now = std::chrono::steady_clock::now();
  auto expired = sessions.insert_unlocked("expired", {});
  expired->second.expires_at = now;
  auto completion = expired->second.completion;
  sessions.insert_unlocked("live", {});
  sessions.expire_unlocked(now);
  EXPECT_EQ(completion->wait(now), false);
  EXPECT_EQ(sessions.entries.size(), 1);
  EXPECT_EQ(sessions.pending_unlocked()->first, "live");
}

TEST(PairingState, WaitRequiresFinalResultAndFirstResultWins) {
  pairing::completion_t completion;
  EXPECT_FALSE(completion.wait(std::chrono::steady_clock::now()).has_value());
  auto waiter = std::async(std::launch::async, [&] {
    return completion.wait(std::chrono::steady_clock::now() + 1s);
  });
  EXPECT_EQ(waiter.wait_for(10ms), std::future_status::timeout);
  completion.finish(true);
  completion.finish(false);
  EXPECT_EQ(waiter.get(), true);
}

TEST(PairingState, ConcurrentRetriesAndExpiryAreSerialized) {
  pairing::sessions_t<pairing::state_t> sessions;
  auto retry = [&] {
    for (int i = 0; i < 100; ++i) {
      std::lock_guard lock {sessions.mutex};
      sessions.insert_unlocked("client", {});
      sessions.expire_unlocked(std::chrono::steady_clock::now());
      auto it = sessions.pending_unlocked();
      ASSERT_NE(it, sessions.entries.end());
      it->second.fail();
      sessions.entries.erase(it);
    }
  };
  std::thread first {retry};
  std::thread second {retry};
  first.join();
  second.join();
  EXPECT_TRUE(sessions.entries.empty());
}
