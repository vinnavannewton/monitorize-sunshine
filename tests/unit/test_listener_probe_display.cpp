/**
 * @file tests/unit/test_listener_probe_display.cpp
 * @brief Test listener encoding devices without requiring a display or GPU.
 */
// test includes
#include "../tests_common.h"

// local includes
#ifdef __linux__
  #include "src/listener_probe_display.h"

/**
 * @brief Software probes retain the generic RAM conversion device.
 */
TEST(ListenerProbeDisplayTest, ProvidesSystemMemoryDevice) {
  video::listener_probe_display display {platf::mem_type_e::system};
  auto device = display.make_avcodec_encode_device(platf::pix_fmt_e::nv12);
  ASSERT_NE(device, nullptr);
  EXPECT_FALSE(device->hardware);
}

#ifndef SUNSHINE_BUILD_CUDA
/**
 * @brief NVENC probes use RAM conversion when Sunshine CUDA kernels are absent.
 */
TEST(ListenerProbeDisplayTest, ProvidesNvencRamFallbackWithoutCudaBuildSupport) {
  video::listener_probe_display display {platf::mem_type_e::cuda};
  for (auto format : {platf::pix_fmt_e::nv12, platf::pix_fmt_e::p010, platf::pix_fmt_e::yuv444p, platf::pix_fmt_e::yuv444p16}) {
    auto device = display.make_avcodec_encode_device(format);
    ASSERT_NE(device, nullptr);
    EXPECT_FALSE(device->hardware);
    EXPECT_EQ(device->init_hw_device, nullptr);
  }
}
#endif

/**
 * @brief Unrecognized device types remain unavailable rather than passing preflight.
 */
TEST(ListenerProbeDisplayTest, RejectsUnsupportedDevice) {
  video::listener_probe_display display {platf::mem_type_e::unknown};
  EXPECT_EQ(display.make_avcodec_encode_device(platf::pix_fmt_e::nv12), nullptr);
}
#endif
