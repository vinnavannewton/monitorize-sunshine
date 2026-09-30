/**
 * @file tests/unit/platform/linux/test_pipewire_target.cpp
 * @brief Regression tests for PipeWire node-ID and object-serial targeting.
 */
#ifdef SUNSHINE_BUILD_PIPEWIRE_NODE
  #include <gtest/gtest.h>
  #include <src/platform/linux/pipewire_target.h>

  namespace {
    /** @brief Own fresh PipeWire stream properties for each target-selection test. */
    class PipeWireTargetTest: public testing::Test {
    protected:
      void SetUp() override {
        props = pw_properties_new("media.type", "Video", nullptr);
        ASSERT_NE(props, nullptr);
      }
      void TearDown() override {
        pw_properties_free(props);
      }
      pw_properties *props = nullptr;  ///< Properties passed to production target configuration.
      uint32_t target = PW_ID_ANY;  ///< Target argument passed to pw_stream_connect.
    };

    TEST_F(PipeWireTargetTest, SerialTakesPrecedenceOverDifferentNodeId) {
      ASSERT_TRUE(pipewire::configure_capture_target(props, 60, 156, target));
      EXPECT_EQ(target, PW_ID_ANY);
      EXPECT_STREQ(pw_properties_get(props, "target.object"), "156");
      EXPECT_EQ(pw_properties_get(props, "node.target"), nullptr);
    }

    TEST_F(PipeWireTargetTest, MissingSerialUsesNodeIdWithoutExplicitProperties) {
      for (uint64_t serial : {uint64_t(0), uint64_t(SPA_ID_INVALID)}) {
        ASSERT_TRUE(pipewire::configure_capture_target(props, 60, serial, target));
        EXPECT_EQ(target, 60u);
        EXPECT_EQ(pw_properties_get(props, "target.object"), nullptr);
        EXPECT_EQ(pw_properties_get(props, "node.target"), nullptr);
      }
    }

    TEST_F(PipeWireTargetTest, EqualIdentifiersStillUseSerial) {
      ASSERT_TRUE(pipewire::configure_capture_target(props, 60, 60, target));
      EXPECT_EQ(target, PW_ID_ANY);
      EXPECT_STREQ(pw_properties_get(props, "target.object"), "60");
      EXPECT_EQ(pw_properties_get(props, "node.target"), nullptr);
    }

    TEST_F(PipeWireTargetTest, LargeSerialIsPreservedWithoutNodeId) {
      ASSERT_TRUE(pipewire::configure_capture_target(props, PW_ID_ANY, UINT64_C(4294967452), target));
      EXPECT_EQ(target, PW_ID_ANY);
      EXPECT_STREQ(pw_properties_get(props, "target.object"), "4294967452");
      EXPECT_EQ(pw_properties_get(props, "node.target"), nullptr);
    }

    TEST_F(PipeWireTargetTest, MissingIdentifiersAreRejected) {
      for (uint32_t node : {uint32_t(0), uint32_t(PW_ID_ANY)}) {
        for (uint64_t serial : {uint64_t(0), uint64_t(SPA_ID_INVALID)}) {
          EXPECT_FALSE(pipewire::configure_capture_target(props, node, serial, target));
          EXPECT_EQ(pw_properties_get(props, "target.object"), nullptr);
          EXPECT_EQ(pw_properties_get(props, "node.target"), nullptr);
        }
      }
    }

    TEST_F(PipeWireTargetTest, MissingPropertiesAreRejected) {
      EXPECT_FALSE(pipewire::configure_capture_target(nullptr, 60, 156, target));
    }
  }  // namespace
#endif
