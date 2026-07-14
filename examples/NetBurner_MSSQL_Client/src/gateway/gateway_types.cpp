// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_types.h"

namespace gateway {

const char *HandshakeStateName(HandshakeState s)
{
    switch (s) {
    case HandshakeState::Initializing:
        return "Initializing";
    case HandshakeState::Idle:
        return "Idle";
    case HandshakeState::CapturePending:
        return "CapturePending";
    case HandshakeState::Capturing:
        return "Capturing";
    case HandshakeState::Persisting:
        return "Persisting";
    case HandshakeState::AssertAck:
        return "AssertAck";
    case HandshakeState::WaitForTriggerLow:
        return "WaitForTriggerLow";
    case HandshakeState::ClearAck:
        return "ClearAck";
    case HandshakeState::VerifyAckLow:
        return "VerifyAckLow";
    case HandshakeState::Backoff:
        return "Backoff";
    case HandshakeState::Faulted:
        return "Faulted";
    default:
        return "Unknown";
    }
}

} // namespace gateway
