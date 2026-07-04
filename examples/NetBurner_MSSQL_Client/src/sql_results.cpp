// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "sql_results.h"

#include <stdio.h>
#include <string.h>

#include <tdslite/detail/sqltypes/sql_datetime.hpp>
#include <tdslite/detail/sqltypes/sql_smalldatetime.hpp>

static sql_result_store g_results = {};

static void CopyU16Ascii(char *dst, tdsl::size_t cap, tdsl::u16char_view text)
{
    if (cap == 0) {
        return;
    }

    tdsl::size_t out = 0;
    for (auto ch : text) {
        if (ch == 0 || out + 1 >= cap) {
            break;
        }
        if (ch <= 0x7F) {
            dst[out++] = static_cast<char>(ch);
        }
        else {
            dst[out++] = '?';
        }
    }
    dst[out] = '\0';
}

static void CopyLatin1Field(char *dst, tdsl::size_t cap, tdsl::byte_view bytes)
{
    if (cap == 0) {
        return;
    }

    tdsl::size_t out = 0;
    for (tdsl::size_t i = 0; i < bytes.size_bytes() && out + 1 < cap; ++i) {
        const char c = static_cast<char>(bytes.data()[i]);
        if (c == '\0') {
            break;
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
}

static void CopyUtf16LeAsciiField(char *dst, tdsl::size_t cap, tdsl::byte_view bytes)
{
    if (cap == 0) {
        return;
    }

    tdsl::size_t out = 0;
    for (tdsl::size_t i = 0; (i + 1) < bytes.size_bytes() && out + 1 < cap; i += 2) {
        const char c = static_cast<char>(bytes.data()[i]);
        if (c == '\0') {
            break;
        }
        dst[out++] = c;
    }
    dst[out] = '\0';
}

static void DaysSince1900ToYmd(tdsl::int32_t days, int & year, int & month, int & day)
{
    year                   = 1900;
    tdsl::int32_t remaining = days;

    while (remaining >= 365) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        const tdsl::int32_t year_days = leap ? 366 : 365;
        if (remaining < year_days) {
            break;
        }
        remaining -= year_days;
        ++year;
    }

    static const tdsl::uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    month                                      = 1;
    while (month <= 12) {
        tdsl::int32_t month_days = days_in_month[month - 1];
        if (month == 2) {
            const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            if (leap) {
                month_days = 29;
            }
        }
        if (remaining < month_days) {
            break;
        }
        remaining -= month_days;
        ++month;
    }

    day = remaining + 1;
}

static void FormatSqlDateTime(char *dst, tdsl::size_t cap, const tdsl::sql_datetime & dt)
{
    int year = 0;
    int month = 0;
    int day = 0;
    DaysSince1900ToYmd(dt.days_elapsed, year, month, day);

    const tdsl::uint32_t ticks300   = dt.centiseconds_elapsed;
    const tdsl::uint32_t total_sec  = ticks300 / 300u;
    const tdsl::uint32_t ms         = (ticks300 % 300u) * 1000u / 300u;
    const tdsl::uint32_t hour       = total_sec / 3600u;
    const tdsl::uint32_t minute     = (total_sec % 3600u) / 60u;
    const tdsl::uint32_t second     = total_sec % 60u;

    snprintf(dst, cap, "%04d-%02d-%02d %02u:%02u:%02u.%03u", year, month, day, hour, minute,
             second, ms);
}

static void FormatSqlSmallDateTime(char *dst, tdsl::size_t cap, const tdsl::sql_smalldatetime & dt)
{
    int year = 0;
    int month = 0;
    int day = 0;
    DaysSince1900ToYmd(dt.days_elapsed, year, month, day);

    const tdsl::uint32_t hour   = dt.minutes_elapsed / 60u;
    const tdsl::uint32_t minute = dt.minutes_elapsed % 60u;

    snprintf(dst, cap, "%04d-%02d-%02d %02u:%02u:00", year, month, day, hour, minute);
}

static void FormatField(char *dst, tdsl::size_t cap, const tdsl::tdsl_field & field,
                        const tdsl::tds_column_info & col)
{
    using dt = tdsl::detail::e_tds_data_type;

    if (cap == 0) {
        return;
    }

    if (field.is_null()) {
        strncpy(dst, "NULL", cap - 1);
        dst[cap - 1] = '\0';
        return;
    }

    switch (col.type) {
        case dt::INT1TYPE:
            snprintf(dst, cap, "%d", static_cast<int>(field.as<tdsl::int8_t>()));
            break;
        case dt::BITTYPE:
        case dt::BITNTYPE:
            snprintf(dst, cap, "%u", static_cast<unsigned>(field.as<tdsl::uint8_t>()));
            break;
        case dt::INT2TYPE:
            snprintf(dst, cap, "%d", static_cast<int>(field.as<tdsl::int16_t>()));
            break;
        case dt::INT4TYPE:
            snprintf(dst, cap, "%ld", static_cast<long>(field.as<tdsl::int32_t>()));
            break;
        case dt::INTNTYPE:
            switch (col.typeprops.u8l.length) {
                case 1:
                    snprintf(dst, cap, "%d", static_cast<int>(field.as<tdsl::int8_t>()));
                    break;
                case 2:
                    snprintf(dst, cap, "%d", static_cast<int>(field.as<tdsl::int16_t>()));
                    break;
                case 4:
                    snprintf(dst, cap, "%ld", static_cast<long>(field.as<tdsl::int32_t>()));
                    break;
                case 8:
                    snprintf(dst, cap, "%lld", static_cast<long long>(field.as<tdsl::int64_t>()));
                    break;
                default:
                    snprintf(dst, cap, "?int%u", static_cast<unsigned>(col.typeprops.u8l.length));
                    break;
            }
            break;
        case dt::INT8TYPE:
            snprintf(dst, cap, "%lld", static_cast<long long>(field.as<tdsl::int64_t>()));
            break;
        case dt::FLT4TYPE:
            snprintf(dst, cap, "%g", static_cast<double>(field.as<float>()));
            break;
        case dt::FLTNTYPE:
            switch (col.typeprops.u8l.length) {
                case 4:
                    snprintf(dst, cap, "%g", static_cast<double>(field.as<float>()));
                    break;
                case 8:
                    snprintf(dst, cap, "%g", field.as<double>());
                    break;
                default:
                    snprintf(dst, cap, "?flt%u", static_cast<unsigned>(col.typeprops.u8l.length));
                    break;
            }
            break;
        case dt::FLT8TYPE:
            snprintf(dst, cap, "%g", field.as<double>());
            break;
        case dt::NVARCHARTYPE:
        case dt::NCHARTYPE:
            CopyUtf16LeAsciiField(dst, cap, field.as<tdsl::byte_view>());
            break;
        case dt::BIGVARCHRTYPE:
        case dt::BIGCHARTYPE:
            CopyLatin1Field(dst, cap, field.as<tdsl::byte_view>());
            break;
        case dt::GUIDTYPE: {
            const auto bytes = field.as<tdsl::byte_view>();
            tdsl::size_t out   = 0;
            for (tdsl::size_t i = 0; i < bytes.size_bytes() && out + 2 < cap; ++i) {
                out += static_cast<tdsl::size_t>(
                    snprintf(dst + out, cap - out, "%02X", bytes.data()[i]));
            }
            break;
        }
        case dt::DATETIMETYPE:
            FormatSqlDateTime(dst, cap, field.as<tdsl::sql_datetime>());
            break;
        case dt::DATETIM4TYPE:
            FormatSqlSmallDateTime(dst, cap, field.as<tdsl::sql_smalldatetime>());
            break;
        case dt::DATETIMNTYPE:
            switch (col.typeprops.u8l.length) {
                case 4:
                    FormatSqlSmallDateTime(dst, cap, field.as<tdsl::sql_smalldatetime>());
                    break;
                case 8:
                    FormatSqlDateTime(dst, cap, field.as<tdsl::sql_datetime>());
                    break;
                default:
                    snprintf(dst, cap, "?dt%u", static_cast<unsigned>(col.typeprops.u8l.length));
                    break;
            }
            break;
        default:
            snprintf(dst, cap, "0x%02X", static_cast<unsigned>(col.type));
            break;
    }
}

void SqlResultsReset()
{
    memset(&g_results, 0, sizeof(g_results));
    strncpy(g_results.status, "Connecting to SQL Server...", sizeof(g_results.status) - 1);
}

sql_result_store & SqlResultsGet()
{
    return g_results;
}

static void SqlResultsRowCallbackImpl(void *user_ptr, const tdsl::tds_colmetadata_token & colmd,
                                      const tdsl::tdsl_row & row)
{
    (void) user_ptr;
    auto & store = g_results;

    if (store.row_count == 0 && store.col_count == 0) {
        store.col_count = colmd.columns.size();
        if (store.col_count > SQL_RESULT_MAX_COLS) {
            store.col_count = SQL_RESULT_MAX_COLS;
        }

        for (tdsl::uint32_t i = 0; i < store.col_count; ++i) {
            if (colmd.column_names && colmd.column_names[i].size() > 0) {
                CopyU16Ascii(store.col_names[i], sizeof(store.col_names[i]),
                             colmd.column_names[i]);
            }
            else {
                snprintf(store.col_names[i], sizeof(store.col_names[i]), "col%u",
                         static_cast<unsigned>(i));
            }
        }
    }

    if (store.row_count >= SQL_RESULT_MAX_ROWS) {
        return;
    }

    tdsl::uint32_t col_idx = 0;
    for (const auto & field : row) {
        if (col_idx >= store.col_count) {
            break;
        }
        FormatField(store.cells[store.row_count][col_idx], SQL_RESULT_CELL_LEN, field,
                    colmd.columns[col_idx]);
        ++col_idx;
    }

    ++store.row_count;
}

tdsl::netburner_driver::sql_command_row_callback SqlResultsRowCallback()
{
    return +[](void * uptr, const tdsl::tds_colmetadata_token & colmd,
               const tdsl::tdsl_row & row) -> void {
        SqlResultsRowCallbackImpl(uptr, colmd, row);
    };
}
