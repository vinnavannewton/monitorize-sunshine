#pragma once
// Linux-only encoder probing. No compositor, DRM output, input or real capture.
#include <vector>
#include "platform/common.h"
#ifdef SUNSHINE_BUILD_VAAPI
#include "platform/linux/vaapi.h"
#endif
#ifdef SUNSHINE_BUILD_CUDA
#include "platform/linux/cuda.h"
#endif

namespace video {
  class listener_probe_display final : public platf::display_t {
    platf::mem_type_e type;
    struct image final : platf::img_t {
      std::vector<std::uint8_t> storage;
      image(int w, int h) : storage(w * h * 4, 0) {
        width=w; height=h; pixel_pitch=4; row_pitch=w*4; data=storage.data();
      }
    };
  public:
    explicit listener_probe_display(platf::mem_type_e t) : type(t) {
      width=env_width=1920; height=env_height=1080;
    }
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::capture_error; // This object must never capture a stream.
    }
    std::shared_ptr<platf::img_t> alloc_img() override { return std::make_shared<image>(width,height); }
    int dummy_img(platf::img_t *) override { return 0; }
    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (type==platf::mem_type_e::vaapi) return va::make_avcodec_encode_device(width,height,false);
#endif
#ifdef SUNSHINE_BUILD_CUDA
      if (type==platf::mem_type_e::cuda) return cuda::make_avcodec_encode_device(width,height,false);
#endif
      if (type==platf::mem_type_e::system) return std::make_unique<platf::avcodec_encode_device_t>();
      return nullptr;
    }
  };
}
