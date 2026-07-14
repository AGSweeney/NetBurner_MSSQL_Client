// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
//
// Onboard EFFS-STD mount for the gateway durable queue.

#ifndef GATEWAY_EFFS_H
#define GATEWAY_EFFS_H

namespace gateway {

// Mount EFFS-STD (drive 0), ensure SQLQUEUE/ exists.
// Returns true when flash FS is usable. On failure the queue falls back to RAM.
bool EffsGatewayInit();

bool EffsGatewayIsMounted();

// Serialize all EFFS I/O used by the queue.
void EffsLock();
void EffsUnlock();

} // namespace gateway

#endif
