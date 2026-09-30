/**
 * @file src/platform/linux/pipewire_target.h
 * @brief Select PipeWire capture targets without mixing node IDs and serials.
 */
#pragma once

#include <cinttypes>
#include <pipewire/pipewire.h>

namespace pipewire {
  /**
   * @brief Configure explicit target properties and the stream connection target.
   *
   * @param props Fresh stream properties to configure.
   * @param node Target node ID, or PW_ID_ANY when unavailable.
   * @param serial Target object serial, or zero/SPA_ID_INVALID when unavailable.
   * @param target_id Receives the target argument for pw_stream_connect.
   * @return True when a usable target was configured; false on invalid input or allocation failure.
   */
  inline bool configure_capture_target(pw_properties *props, uint32_t node, uint64_t serial, uint32_t &target_id) {
    if (!props) {
      return false;
    }
    // Keep the existing portal sentinel interpretation. Node IDs and object
    // serials are distinct: target.object must never receive a node ID.
    if (serial != 0 && (serial & SPA_ID_INVALID) != SPA_ID_INVALID) {
      target_id = PW_ID_ANY;
      return pw_properties_setf(props, "target.object", "%" PRIu64, serial) >= 0;
    }
    if (node != PW_ID_ANY && node != 0) {
      // PipeWire handles the legacy node-ID argument itself. Do not override
      // its targeting with an explicit target.object or node.target property.
      target_id = node;
      return true;
    }
    return false;
  }
}  // namespace pipewire
