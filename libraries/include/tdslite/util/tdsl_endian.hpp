/**
 * ____________________________________________________
 * Endianness related definitions
 *
 * @file   tds_endian.hpp
 * @author mkg <me@mustafagilor.com>
 * @date   12.04.2022
 *
 * SPDX-License-Identifier:    MIT
 * ____________________________________________________
 */

#ifndef TDSL_UTIL_TDS_ENDIAN_HPP
#define TDSL_UTIL_TDS_ENDIAN_HPP

namespace tdsl {

#if !defined(_WIN32) && !defined(TDSL_FORCE_LITTLE_ENDIAN) && !defined(TDSL_FORCE_BIG_ENDIAN)
#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__)
#error \
    "tdslite requires GCC/Clang byte-order macros (__BYTE_ORDER__). On NetBurner ColdFire big-endian targets (NANO54415, MOD54415) and ARM little-endian targets (SOMRT1061, MODRT1171, MODM7AE70) these are normally provided by the toolchain. For host testing, define TDSL_FORCE_LITTLE_ENDIAN or TDSL_FORCE_BIG_ENDIAN."
#endif
#endif

    /**
     * Endianness kinds
     */
    enum class endian
    {
#if defined(TDSL_FORCE_LITTLE_ENDIAN)
        little     = 0,
        big        = 1,
        native     = little,
        non_native = big
#elif defined(TDSL_FORCE_BIG_ENDIAN)
        little     = 0,
        big        = 1,
        native     = big,
        non_native = little
#elif defined(_WIN32)
        little     = 0,
        big        = 1,
        native     = little,
        non_native = big
#else
        little     = __ORDER_LITTLE_ENDIAN__,
        big        = __ORDER_BIG_ENDIAN__,
        native     = __BYTE_ORDER__,
        non_native = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? __ORDER_BIG_ENDIAN__
                                                                : __ORDER_LITTLE_ENDIAN__)
#endif
    };

} // namespace tdsl

#endif