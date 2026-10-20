#pragma once
#include <cstdint>

namespace listener_codec {
  inline constexpr char protocol_marker[] = "MONITORIZE_CODEC_NEGOTIATION=1";
  // Internal bits follow the negotiated bitStreamFormat values, not SCM bits.
  constexpr int forced_format(int hevc, int av1) {
    return av1 >= 2 ? 2 : hevc >= 2 ? 1 : (hevc == 1 && av1 == 1) ? 0 : -1;
  }
  constexpr unsigned advertise(unsigned tested, int hevc, int av1) {
    auto forced = forced_format(hevc, av1);
    if (forced >= 0 && !(tested & (1U << forced))) return 0;
    // H.264 remains the protocol baseline, but RTSP strictly rejects mismatches.
    return forced < 0 ? tested : (tested & (1U << forced)) | (tested & 1U);
  }
  constexpr bool accepts(int requested, int hevc, int av1) {
    return requested >= 0 && requested <= 2 &&
      (forced_format(hevc, av1) < 0 || requested == forced_format(hevc, av1));
  }
}
