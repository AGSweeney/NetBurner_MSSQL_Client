/**
 * Compile-time and runtime checks that tdslite's endian helpers match the host.
 *
 * TDS wire data is little-endian; binary_reader<little> / write_le() rely on
 * tdsl::endian::native matching __BYTE_ORDER__ (or TDSL_FORCE_* overrides).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TDSL_UTIL_TDSL_ENDIAN_CHECK_HPP
#define TDSL_UTIL_TDSL_ENDIAN_CHECK_HPP

#include <tdslite/util/tdsl_byte_swap.hpp>
#include <tdslite/util/tdsl_endian.hpp>
#include <tdslite/util/tdsl_inttypes.hpp>
#include <tdslite/util/tdsl_macrodef.hpp>

namespace tdsl {

    inline TDSL_NODISCARD constexpr bool endian_is_little() noexcept {
        return tdsl::endian::native == tdsl::endian::little;
    }

    inline TDSL_NODISCARD constexpr bool endian_is_big() noexcept {
        return tdsl::endian::native == tdsl::endian::big;
    }

#if defined(TDSL_FORCE_LITTLE_ENDIAN)
    static_assert(endian_is_little(), "TDSL_FORCE_LITTLE_ENDIAN is set but native is not little");
#elif defined(TDSL_FORCE_BIG_ENDIAN)
    static_assert(endian_is_big(), "TDSL_FORCE_BIG_ENDIAN is set but native is not big");
#elif defined(_WIN32)
    static_assert(endian_is_little(), "Windows builds assume little-endian host");
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    static_assert(endian_is_little(), "tdsl::endian::native must match __BYTE_ORDER__");
#else
    static_assert(endian_is_big(), "tdsl::endian::native must match __BYTE_ORDER__");
#endif
#endif

    namespace detail {

        inline TDSL_NODISCARD bool probe_host_is_little() noexcept {
            const tdsl::uint16_t one = 1;
            return *reinterpret_cast<const tdsl::uint8_t *>(&one) == 1;
        }

    } // namespace detail

    inline TDSL_NODISCARD bool endian_self_test() noexcept {
        if (detail::probe_host_is_little() != endian_is_little()) {
            return false;
        }

        const tdsl::uint32_t v = 0x01020304u;
        if (le_to_native(native_to_le(v)) != v) {
            return false;
        }

        const double f = 30.6;
        if (le_to_native(native_to_le(f)) != f) {
            return false;
        }

        return true;
    }

} // namespace tdsl

#endif
