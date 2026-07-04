<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
-->

# NetBurner compatibility patches

The vendored tdslite headers include small patches required for the NetBurner
GCC toolchain (`-fno-exceptions`, `-fno-rtti`, gnu++17). When updating from
upstream, re-apply these changes if the corresponding files changed.

## tdslite/util/tdsl_byte_swap.hpp

Add explicit `byte_swap<char>` specialization. NetBurner GCC instantiates
`byte_swap(char)` but `char` is distinct from `int8_t`/`uint8_t` on this toolchain.

Add explicit `byte_swap<char16_t>` specialization. LOGIN7 UTF-16 string fields use
`write(char16_t)`, which byte-swaps through `native_to_le()` on big-endian ColdFire.

Add explicit `byte_swap<float>` and `byte_swap<double>` specializations. Result-set
field decoding uses `binary_reader<little>` on big-endian ColdFire.

## tdslite/detail/tdsl_callback.hpp

Replace `return_type{}` with `return_type()` in the callback fallback return.
Avoids an internal compiler error when `return_type` is `packet_handler_result<...>`.

## tdslite/detail/tdsl_packet_handler_result.hpp

Replace in-class default member initializers with an explicit default constructor.
Avoids GCC ICE in `gimplify_init_constructor` when the status type is an
`enum class` used as a non-type template parameter default.

## tdslite-net/base/network_io_base.hpp

Check `do_recv()` result before parsing the TDS header. Without this, a failed read
leaves a zero-filled buffer and triggers a misleading "Invalid tds message length!" assert.

Replace the invalid-length `TDSL_ASSERT_MSG` with a debug print and early return so
embedded firmware reports a login failure instead of trapping.

In the partial TDS receive loop, never pass `get_writer()->remaining_bytes()` directly
into `do_recv()`. The temporary writer stays alive for the full call expression while
`do_recv()` acquires a second writer, triggering "Buffer object is already in use!"
on wide result sets (e.g. TrayRecords). Use scoped helper methods instead.

## tdslite/detail/tdsl_tds_context.hpp

Add `exchange_prelogin()` and call it from `login_context::do_login()` before LOGIN7.
SQL Server 2019+ closes TCP (`TCP_ERR_CLOSING`) when PRELOGIN is skipped.

## tdslite/detail/tdsl_login_context.hpp

Invoke `tds_ctx.exchange_prelogin()` at the start of `do_login()`.

## tdslite/detail/tdsl_net_tx_mixin.hpp

Route multibyte integral writes through `native_to_le()` before copying to the
network buffer. Upstream `write(T)` used host-endian `memcpy`, which breaks
LOGIN7 on big-endian ColdFire (NANO54415, MOD54415): UTF-16 string fields, offset/length
tables, and other little-endian fields were sent byte-swapped. `write_be()` now
uses a dedicated `write_native_bytes()` path so TDS-version fields stay big-endian.

Add `write(float)` / `write(double)` overloads routed through `write_le()`.

## tdslite/detail/tdsl_string_writer.hpp

Write wide (`wstring_view`) LOGIN/query strings one UTF-16 code unit at a time
via `write(char16_t)` instead of bulk `write(span)`. Bulk span write copies host
byte order and breaks wide-string packets on big-endian ColdFire.

## tdslite/util/tdsl_endian.hpp

Document big-endian ColdFire targets (NANO54415, MOD54415, MOD5441X) and
little-endian ARM targets (SOMRT1061, MODRT1171, MODM7AE70).
Add optional `TDSL_FORCE_LITTLE_ENDIAN` / `TDSL_FORCE_BIG_ENDIAN` for host-side
simulation tests. Fail the build with a clear `#error` when `__BYTE_ORDER__` is
missing.

## tdslite/util/tdsl_endian_check.hpp

New header: compile-time and `constexpr` runtime self-test for
`native_to_le()` / `le_to_native()` on integers and IEEE floats. Included from
`TDSLite/tdslite.h` so every firmware build verifies endian helpers.

## tdslite-net/netburner/tdsl_netimpl_netburner.hpp

NetBurner-specific adapter (not in upstream):

- Use `SockReadWithTimeout()` in a loop for exact-length TCP reads
- Verify `writeall()` results; coalesce header + body into one write when possible
- Log read/write failures via `printf()` for the web console
- Scope `progressive_binary_writer` in `do_connect()` so the network buffer `in_use`
  flag is never held across nested `get_writer()` calls (fixes "Buffer object is already
  in use!" assertion on connect/query)

## tdslite/detail/tdsl_driver.hpp

Add public `disconnect()` to close the TCP socket between web-triggered queries.
