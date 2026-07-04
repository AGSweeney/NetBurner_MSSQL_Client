/**
 * ____________________________________________________
 * Network implementation for tdslite on NetBurner.
 *
 * Uses the NetBurner TCP client API (connect, read, write)
 * with DNS resolution via GetHostByName.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
 * ____________________________________________________
 */

#ifndef TDSL_NET_NETIMPL_NETBURNER_HPP
#define TDSL_NET_NETIMPL_NETBURNER_HPP

#include <tdslite-net/base/network_io_base.hpp>

#include <tdslite/util/tdsl_span.hpp>
#include <tdslite/util/tdsl_macrodef.hpp>
#include <tdslite/util/tdsl_expected.hpp>

#include <dns.h>
#include <iosys.h>
#include <nbrtos.h>
#include <stdio.h>
#include <tcp.h>

namespace tdsl { namespace net {

    /**
     * Synchronous networking for NetBurner TCP sockets.
     */
    struct tdsl_netimpl_netburner : public network_io_base<tdsl_netimpl_netburner> {

        using network_io_result =
            typename network_io_base<tdsl_netimpl_netburner>::network_io_result;

        static constexpr unsigned long k_default_connect_timeout_ticks = TICKS_PER_SECOND * 10;
        static constexpr unsigned long k_default_recv_timeout_ticks    = TICKS_PER_SECOND * 30;

        // --------------------------------------------------------------------------------

        template <tdsl::uint32_t BufSize>
        inline tdsl_netimpl_netburner(tdsl::uint8_t (&network_io_buffer)[BufSize]) noexcept :
            fd(-1) {
            this->network_buffer = tdsl::tdsl_buffer_object{network_io_buffer};
        }

        // --------------------------------------------------------------------------------

        inline tdsl_netimpl_netburner(tdsl::byte_span network_io_buffer) noexcept : fd(-1) {
            this->network_buffer = tdsl::tdsl_buffer_object{network_io_buffer};
        }

        // --------------------------------------------------------------------------------

        TDSL_SYMBOL_VISIBLE tdsl::expected<tdsl::traits::true_type, int>
        do_connect(tdsl::char_view target, tdsl::uint16_t port) {
            do_disconnect();

            tdsl::char_view destination_host{};

            {
                auto writer = this->network_buffer.get_writer();
                writer->reset();

                if (target.size_bytes() > writer->remaining_bytes()) {
                    return tdsl::unexpected(-99);
                }

                for (auto c : target) {
                    (void) writer->write(c);
                }

                destination_host = writer->inuse_span().template rebind_cast<const char>();
            }

            IPADDR server_ip{};
            const auto dns_result =
                GetHostByName(destination_host.data(), &server_ip, IPADDR::NullIP(), dns_timeout_ticks);

            if (dns_result != DNS_OK) {
                auto writer = this->network_buffer.get_writer();
                writer->reset();
                return tdsl::unexpected(static_cast<int>(dns_result));
            }

            int cr      = 0;
            int retries = static_cast<int>(this->conn_retry_count);

            while (retries-- > 0) {
                TDSL_DEBUG_PRINTLN("... attempting to connect to %s:%d, %d retries remaining ...",
                                   destination_host.data(), port, retries);
                cr = ::connect(server_ip, port, connect_timeout_ticks);
                if (cr > 0) {
                    fd = cr;
                    break;
                }

                TDSL_DEBUG_PRINTLN("... connection attempt failed (%d) ...", cr);
                if (retries > 0) {
                    OSTimeDly(ms_to_ticks(this->conn_retry_delay_ms));
                }
            }

            {
                auto writer = this->network_buffer.get_writer();
                writer->reset();
            }

            if (cr > 0) {
                return tdsl::traits::true_type{};
            }
            return tdsl::unexpected(cr);
        }

        // --------------------------------------------------------------------------------

        TDSL_SYMBOL_VISIBLE tdsl::int32_t do_disconnect() noexcept {
            if (fd > 0) {
                close(fd);
                fd = -1;
            }
            return 0;
        }

        // --------------------------------------------------------------------------------

        TDSL_SYMBOL_VISIBLE void do_send(byte_view header, byte_view message) noexcept {
            if (fd <= 0) {
                return;
            }

            const auto total_bytes = header.size_bytes() + message.size_bytes();
            if (total_bytes == 0) {
                return;
            }

            if (total_bytes <= sizeof(send_scratch)) {
                tdsl::size_t offset = 0;
                if (header.size_bytes() > 0) {
                    for (tdsl::size_t i = 0; i < header.size_bytes(); ++i) {
                        send_scratch[i] = header.data()[i];
                    }
                    offset = header.size_bytes();
                }
                if (message.size_bytes() > 0) {
                    for (tdsl::size_t i = 0; i < message.size_bytes(); ++i) {
                        send_scratch[offset + i] = message.data()[i];
                    }
                }

                const int written =
                    writeall(fd, reinterpret_cast<const char *>(send_scratch),
                             static_cast<int>(total_bytes));
                if (written != static_cast<int>(total_bytes)) {
                    printf("TDSLite: TCP write failed (%d of %u bytes)\r\n", written,
                           static_cast<unsigned>(total_bytes));
                }
                return;
            }

            if (header.size_bytes() > 0) {
                const int written =
                    writeall(fd, reinterpret_cast<const char *>(header.data()),
                             static_cast<int>(header.size_bytes()));
                if (written != static_cast<int>(header.size_bytes())) {
                    printf("TDSLite: TCP header write failed (%d of %u bytes)\r\n", written,
                           static_cast<unsigned>(header.size_bytes()));
                    return;
                }
            }

            if (message.size_bytes() > 0) {
                const int written =
                    writeall(fd, reinterpret_cast<const char *>(message.data()),
                             static_cast<int>(message.size_bytes()));
                if (written != static_cast<int>(message.size_bytes())) {
                    printf("TDSLite: TCP body write failed (%d of %u bytes)\r\n", written,
                           static_cast<unsigned>(message.size_bytes()));
                }
            }
        }

        // --------------------------------------------------------------------------------

        TDSL_SYMBOL_VISIBLE auto do_recv(tdsl::uint32_t transfer_exactly, byte_span dst_buf)
            -> network_io_result {

            enum class errc : int
            {
                disconnected           = -1,
                timeout                = -2,
                not_enough_capacity    = -3,
                unexpected_read_amount = -99
            };

            if (fd <= 0) {
                return tdsl::unexpected(static_cast<int>(errc::disconnected));
            }

            if (transfer_exactly > dst_buf.size_bytes()) {
                return tdsl::unexpected(static_cast<int>(errc::not_enough_capacity));
            }

            tdsl::uint32_t received = 0;
            TickTimeout deadline(recv_timeout_ticks);

            while (received < transfer_exactly) {
                unsigned long remaining = deadline;

                const int read_amount = SockReadWithTimeout(
                    fd, reinterpret_cast<char *>(dst_buf.data()) + received,
                    static_cast<int>(transfer_exactly - received), remaining);

                if (read_amount > 0) {
                    received += static_cast<tdsl::uint32_t>(read_amount);
                    continue;
                }

                if (read_amount == 0) {
                    printf("TDSLite: TCP read timeout (%u of %u bytes)\r\n", received,
                           transfer_exactly);
                    if (received == 0) {
                        return tdsl::unexpected(static_cast<int>(errc::timeout));
                    }
                    return tdsl::unexpected(static_cast<int>(errc::unexpected_read_amount));
                }

                if (read_amount == TCP_ERR_CLOSING) {
                    printf("TDSLite: server closed connection (check SQL Server encryption/PRELOGIN)\r\n");
                } else if (read_amount == TCP_ERR_CON_RESET) {
                    printf("TDSLite: connection reset by SQL Server\r\n");
                } else {
                    printf("TDSLite: TCP read error %d (%u of %u bytes)\r\n", read_amount, received,
                           transfer_exactly);
                }
                do_disconnect();
                return tdsl::unexpected(read_amount);
            }

            return transfer_exactly;
        }

        // --------------------------------------------------------------------------------

        TDSL_SYMBOL_VISIBLE auto do_recv(tdsl::uint32_t transfer_exactly) noexcept
            -> network_io_result {
            auto writer          = this->network_buffer.get_writer();
            const auto rem_space = writer->remaining_bytes();

            if (transfer_exactly > rem_space) {
                TDSL_DEBUG_PRINTLN("tdsl_netimpl_netburner::do_recv(...) -> error, not enough "
                                   "space in recv buffer (%u vs " TDSL_SIZET_FORMAT_SPECIFIER ")",
                                   transfer_exactly, rem_space);
                TDSL_ASSERT(0);
                return tdsl::unexpected(-2);
            }

            auto free_space_span = writer->free_span();
            auto result          = do_recv(transfer_exactly, free_space_span);

            if (result) {
                writer->advance(static_cast<tdsl::int32_t>(transfer_exactly));
            }

            return result;
        }

        // --------------------------------------------------------------------------------

        inline void set_connect_timeout_ticks(unsigned long ticks) noexcept {
            connect_timeout_ticks = ticks;
        }

        inline void set_recv_timeout_ticks(unsigned long ticks) noexcept {
            recv_timeout_ticks = ticks;
        }

        inline void set_dns_timeout_ticks(unsigned long ticks) noexcept {
            dns_timeout_ticks = ticks;
        }

    private:
        // --------------------------------------------------------------------------------

        static unsigned long ms_to_ticks(tdsl::uint32_t ms) noexcept {
            return (static_cast<unsigned long>(ms) * TICKS_PER_SECOND) / 1000UL;
        }

        int fd = -1;

        unsigned long connect_timeout_ticks = k_default_connect_timeout_ticks;
        unsigned long recv_timeout_ticks    = k_default_recv_timeout_ticks;
        unsigned long dns_timeout_ticks     = TICKS_PER_SECOND * 5;

        // Covers a full default TDS PDU (4096) plus header in one writeall().
        tdsl::uint8_t send_scratch[4200] = {};
    };

}} // namespace tdsl::net

#endif
