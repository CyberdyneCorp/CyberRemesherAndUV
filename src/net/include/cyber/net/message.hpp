#pragma once

#include <string>

#include "cyber/net/session.hpp"

namespace cyber::net {

// Command dispatch, split out from the transport so it is pure and unit-
// testable without a socket. Both never throw: malformed JSON or unknown
// commands become an {"type":"error"} response (spec: malformed messages are
// rejected without crashing).

// Validates a hello handshake payload; sets `accept` and returns the response
// payload (welcome, or reject carrying both protocol versions).
[[nodiscard]] std::string processHandshake(const std::string& helloJson, bool& accept);

// Processes one request payload against the session, returning the response
// payload.
[[nodiscard]] std::string processRequest(BridgeSession& session, const std::string& requestJson);

}  // namespace cyber::net
