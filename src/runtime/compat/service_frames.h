#pragma once
// What a connection's service frames are, written once per connection (#568, unit A3).
//
// The bridge forwards the service's frames to the game without saying which wire form they are in. The matchmaker
// connection's form was an assumption in the source. This is the line that makes it a measurement: at the first frame
// the service sends on a connection, one Info line names the connection, its role and the form the frame is in.
//
// Platform neutral and pure: no sockets, no logging, no game state. The PC bridge's own path (ws_bridge.cpp) and the
// session router (session_router.cpp, on PC and on Quest) both write the line with FormatLine, so the text is one.
//
// The form is read from the frame's first bytes and nothing else: the EVR marker (evr_codec.h) is `evr`; anything
// else is `unknown`, because the bridge does not decode a protobuf Envelope and so cannot show that a frame is one.

#include <cstddef>
#include <cstring>
#include <string>

#include "runtime/compat/evr_codec.h"

namespace nevr_service_frames {

enum class Format { Evr, Unknown };

inline Format Classify(const std::string& frame) {
  if (frame.size() >= nevr_evr_codec::kMarkerSize &&
      std::memcmp(frame.data(), nevr_evr_codec::kMarker, nevr_evr_codec::kMarkerSize) == 0) {
    return Format::Evr;
  }
  return Format::Unknown;
}

inline const char* FormatName(Format format) { return format == Format::Evr ? "evr" : "unknown"; }

// The line's text without a tag; the writer adds its own ("[NEVR.WS] "). `role` is the connection's role name
// (nevr_socket_role::RoleName). Only the connection index, the role and the form: no frame bytes, no address.
inline std::string FormatLine(int connIdx, const char* role, const std::string& frame) {
  return std::string("service frames conn=") + std::to_string(connIdx) + " role=" + role +
         " format=" + FormatName(Classify(frame));
}

}  // namespace nevr_service_frames
