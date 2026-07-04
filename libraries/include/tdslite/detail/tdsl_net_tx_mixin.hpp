/**
 * ____________________________________________________
 *
 * @file   tdsl_net_tx_mixin.hpp
 * @author mkg <me@mustafagilor.com>
 * @date   04.10.2022
 *
 * SPDX-License-Identifier:    MIT
 * ____________________________________________________
 */

#ifndef TDSL_DETAIL_NET_SEND_IF_MIXIN_HPP
#define TDSL_DETAIL_NET_SEND_IF_MIXIN_HPP

#include <tdslite/detail/tdsl_message_type.hpp>

#include <tdslite/util/tdsl_inttypes.hpp>
#include <tdslite/util/tdsl_type_traits.hpp>
#include <tdslite/util/tdsl_span.hpp>
#include <tdslite/util/tdsl_byte_swap.hpp>

namespace tdsl { namespace detail {

    // --------------------------------------------------------------------------------

    /**
     * Network packet transmit context
     *
     * @tparam Derived Derived type (CRTP)
     */
    template <typename Derived>
    struct net_tx_mixin {

        // --------------------------------------------------------------------------------

        /**
         * Copy @p v to the network buffer using the host byte order of @p v.
         * Used internally after native_to_le/native_to_be so wire layout is correct
         * on big-endian targets (e.g. ColdFire NANO54415 / MOD54415).
         */
        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_native_bytes(T v) noexcept -> void {
            byte_view data(reinterpret_cast<const tdsl::uint8_t *>(&v), sizeof(T));
            write(data);
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_native_bytes(tdsl::size_t offset, T v) noexcept -> void {
            byte_view data(reinterpret_cast<const tdsl::uint8_t *>(&v), sizeof(T));
            write(offset, data);
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write(T v) noexcept -> void {
            if constexpr (sizeof(T) == 1) {
                write_native_bytes(v);
            }
            else {
                // TDS LOGIN7 and most packet fields are little-endian on the wire.
                write_le(v);
            }
        }

        // --------------------------------------------------------------------------------

        template <typename T,
                  typename traits::enable_if<traits::is_floating_point<T>::value, bool>::type = true>
        inline auto write(T v) noexcept -> void {
            write_le(v);
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_be(T v) noexcept -> void {
            write_native_bytes(native_to_be(v));
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_le(T v) noexcept -> void {
            write_native_bytes(native_to_le(v));
        }

        // --------------------------------------------------------------------------------

        template <typename T,
                  typename traits::enable_if<traits::is_floating_point<T>::value, bool>::type = true>
        inline auto write_le(T v) noexcept -> void {
            const auto le_v = native_to_le(v);
            byte_view data(reinterpret_cast<const tdsl::uint8_t *>(&le_v), sizeof(T));
            write(data);
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write(tdsl::size_t offset, T v) noexcept -> void {
            if constexpr (sizeof(T) == 1) {
                write_native_bytes(offset, v);
            }
            else {
                write_le(offset, v);
            }
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_be(tdsl::size_t offset, T v) noexcept -> void {
            write_native_bytes(offset, native_to_be(v));
        }

        // --------------------------------------------------------------------------------

        template <typename T, traits::enable_when::integral<T> = true>
        inline auto write_le(tdsl::size_t offset, T v) noexcept -> void {
            write_native_bytes(offset, native_to_le(v));
        }

        // --------------------------------------------------------------------------------

        template <typename T>
        inline void write(tdsl::size_t offset, tdsl::span<T> data) noexcept {
            static_cast<Derived &>(*this).do_write(offset, data);
        }

        // --------------------------------------------------------------------------------

        template <typename T>
        inline void write(tdsl::span<T> data) noexcept {
            static_cast<Derived &>(*this).do_write(data);
        }

        // --------------------------------------------------------------------------------

        template <typename... Args>
        inline void send(Args &&... args) noexcept {
            static_cast<Derived &>(*this).do_send(TDSL_FORWARD(args)...);
        }

        // --------------------------------------------------------------------------------

        inline void send_tds_pdu(detail::e_tds_message_type mtype) noexcept {
            static_cast<Derived &>(*this).do_send_tds_pdu(mtype);
        }

        // --------------------------------------------------------------------------------

        template <typename T>
        struct placeholder {

            inline placeholder(net_tx_mixin<Derived> & tx, tdsl::size_t offset) :
                tx(tx), offset(offset) {}

            // --------------------------------------------------------------------------------

            inline void write_be(T v) noexcept {
                tx.write_be(offset, v);
            }

            // --------------------------------------------------------------------------------

            inline void write_le(T v) noexcept {
                tx.write_le(offset, v);
            }

        private:
            net_tx_mixin<Derived> & tx;
            tdsl::size_t offset;
        };

        // --------------------------------------------------------------------------------

        /**
         * Put a placeholder value to current offset in order
         * to be filled later. Use returned placeholder object
         * to substitute the real value.
         *
         * @tparam T Placeholder value type
         *
         * @param [in] v Placeholder value. The actual value is ,
         *               not important, but the type is.
         *
         * @return placeholder<T> Placeholder object
         */
        template <typename T, traits::enable_when::integral<T> = true>
        inline placeholder<T> put_placeholder(T v) noexcept {
            auto beg = static_cast<Derived &>(*this).do_get_write_offset();
            write(v);
            return placeholder<T>{*this, beg};
        }
    };

}} // namespace tdsl::detail

#endif