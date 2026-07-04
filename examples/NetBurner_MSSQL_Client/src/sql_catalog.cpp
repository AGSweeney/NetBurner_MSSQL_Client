// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "sql_catalog.h"



#include <stdio.h>

#include <string.h>



static sql_table_catalog g_catalog     = {};

static sql_database_catalog g_db_catalog = {};

static sql_column_catalog g_column_catalog = {};



static bool IsSafeTableNameChar(char c)

{

    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||

           c == '_';

}



static bool IsSafeDatabaseNameChar(char c)

{

    return IsSafeTableNameChar(c) || c == '-';

}



static bool CopyCatalogName(char * dst, tdsl::size_t cap, const char * text,

                            bool (* is_safe_char)(char))

{

    if (cap == 0 || text == nullptr || is_safe_char == nullptr) {

        return false;

    }



    tdsl::size_t len = 0;

    while (text[len] != '\0' && len + 1 < cap) {

        if (!is_safe_char(text[len])) {

            return false;

        }

        ++len;

    }



    if (text[len] != '\0') {

        return false;

    }



    memcpy(dst, text, len);

    dst[len] = '\0';

    return len > 0;

}



static void CopyFirstColumnAsString(char * dst, tdsl::size_t cap,

                                    const tdsl::tdsl_field & field,

                                    const tdsl::tds_column_info & col)

{

    using dt = tdsl::detail::e_tds_data_type;



    if (cap == 0) {

        return;

    }



    dst[0] = '\0';



    if (field.is_null()) {

        return;

    }



    tdsl::size_t out = 0;



    switch (col.type) {

        case dt::BIGVARCHRTYPE:

        case dt::BIGCHARTYPE:

            for (tdsl::size_t i = 0; i < field.as<tdsl::byte_view>().size_bytes() && out + 1 < cap;

                 ++i) {

                const char c = static_cast<char>(field.as<tdsl::byte_view>().data()[i]);

                if (c == '\0') {

                    break;

                }

                dst[out++] = c;

            }

            break;

        case dt::NVARCHARTYPE:

        case dt::NCHARTYPE:

            for (tdsl::size_t i = 0;

                 (i + 1) < field.as<tdsl::byte_view>().size_bytes() && out + 1 < cap; i += 2) {

                const char c = static_cast<char>(field.as<tdsl::byte_view>().data()[i]);

                if (c == '\0') {

                    break;

                }

                dst[out++] = c;

            }

            break;

        default:

            return;

    }



    dst[out] = '\0';

}



static void ReadFirstColumnName(char * name_buf, tdsl::size_t name_cap,

                                const tdsl::tds_colmetadata_token & colmd,

                                const tdsl::tdsl_row & row)

{

    tdsl::uint32_t col_idx = 0;

    for (const auto & field : row) {

        if (col_idx >= colmd.columns.size()) {

            break;

        }



        CopyFirstColumnAsString(name_buf, name_cap, field, colmd.columns[col_idx]);

        break;

    }

}

static void ReadColumnAsString(char * dst, tdsl::size_t cap, tdsl::uint32_t target_col,
                               const tdsl::tds_colmetadata_token & colmd,
                               const tdsl::tdsl_row & row)
{
    tdsl::uint32_t col_idx = 0;
    for (const auto & field : row) {
        if (col_idx >= colmd.columns.size()) {
            break;
        }

        if (col_idx == target_col) {
            CopyFirstColumnAsString(dst, cap, field, colmd.columns[col_idx]);
            return;
        }

        ++col_idx;
    }
}



void SqlCatalogReset()

{

    memset(&g_catalog, 0, sizeof(g_catalog));

    strncpy(g_catalog.status, "Loading tables...", sizeof(g_catalog.status) - 1);

}



sql_table_catalog & SqlCatalogGet()

{

    return g_catalog;

}



void SqlCatalogInvalidateTables()

{

    g_catalog.ready   = false;

    g_catalog.success = false;

    g_catalog.table_count = 0;

    strncpy(g_catalog.status, "Database changed. Click Browse tables.",

            sizeof(g_catalog.status) - 1);

}



bool SqlCatalogTablesMatchDatabase(const char * database)

{

    if (database == nullptr || database[0] == '\0') {

        return false;

    }



    if (!g_catalog.ready || !g_catalog.success) {

        return true;

    }



    return strcmp(g_catalog.database, database) == 0;

}

void SqlColumnCatalogReset()
{
    memset(&g_column_catalog, 0, sizeof(g_column_catalog));
    strncpy(g_column_catalog.status, "Loading columns...", sizeof(g_column_catalog.status) - 1);
}

sql_column_catalog & SqlColumnCatalogGet()
{
    return g_column_catalog;
}

void SqlColumnCatalogInvalidate()
{
    g_column_catalog.ready        = false;
    g_column_catalog.success      = false;
    g_column_catalog.column_count = 0;
    strncpy(g_column_catalog.status, "Table changed. Click Refresh Columns.",
            sizeof(g_column_catalog.status) - 1);
}

bool SqlColumnCatalogMatches(const char * database, const char * table)
{
    if (database == nullptr || table == nullptr || database[0] == '\0' || table[0] == '\0') {
        return false;
    }

    if (!g_column_catalog.ready || !g_column_catalog.success) {
        return true;
    }

    return strcmp(g_column_catalog.database, database) == 0 &&
           strcmp(g_column_catalog.table, table) == 0;
}



void SqlDatabaseCatalogReset()

{

    memset(&g_db_catalog, 0, sizeof(g_db_catalog));

    strncpy(g_db_catalog.status, "Loading databases...", sizeof(g_db_catalog.status) - 1);

}



sql_database_catalog & SqlDatabaseCatalogGet()

{

    return g_db_catalog;

}



static void SqlCatalogRowCallbackImpl(void * user_ptr, const tdsl::tds_colmetadata_token & colmd,

                                      const tdsl::tdsl_row & row)

{

    (void) user_ptr;

    auto & catalog = g_catalog;



    if (catalog.table_count >= SQL_CATALOG_MAX_TABLES) {

        return;

    }



    char name_buf[SQL_CATALOG_NAME_LEN] = {};

    ReadFirstColumnName(name_buf, sizeof(name_buf), colmd, row);



    if (!CopyCatalogName(catalog.table_names[catalog.table_count], SQL_CATALOG_NAME_LEN, name_buf,

                         IsSafeTableNameChar)) {

        return;

    }



    ++catalog.table_count;

}



static void SqlDatabaseCatalogRowCallbackImpl(void * user_ptr,

                                              const tdsl::tds_colmetadata_token & colmd,

                                              const tdsl::tdsl_row & row)

{

    (void) user_ptr;

    auto & catalog = g_db_catalog;



    if (catalog.database_count >= SQL_CATALOG_MAX_DATABASES) {

        return;

    }



    char name_buf[SQL_CATALOG_NAME_LEN] = {};

    ReadFirstColumnName(name_buf, sizeof(name_buf), colmd, row);



    if (!CopyCatalogName(catalog.database_names[catalog.database_count], SQL_CATALOG_NAME_LEN,

                         name_buf, IsSafeDatabaseNameChar)) {

        return;

    }



    ++catalog.database_count;

}

static void SqlColumnCatalogRowCallbackImpl(void * user_ptr,
                                            const tdsl::tds_colmetadata_token & colmd,
                                            const tdsl::tdsl_row & row)
{
    (void) user_ptr;
    auto & catalog = g_column_catalog;

    if (catalog.column_count >= SQL_CATALOG_MAX_COLUMNS) {
        return;
    }

    char name_buf[SQL_CATALOG_NAME_LEN] = {};
    char type_buf[SQL_CATALOG_TYPE_LEN] = {};
    ReadColumnAsString(name_buf, sizeof(name_buf), 0, colmd, row);
    ReadColumnAsString(type_buf, sizeof(type_buf), 1, colmd, row);

    if (!CopyCatalogName(catalog.column_names[catalog.column_count], SQL_CATALOG_NAME_LEN,
                         name_buf, IsSafeTableNameChar)) {
        return;
    }

    CopyCatalogName(catalog.column_types[catalog.column_count], SQL_CATALOG_TYPE_LEN,
                    type_buf, IsSafeTableNameChar);

    ++catalog.column_count;
}



tdsl::netburner_driver::sql_command_row_callback SqlCatalogRowCallback()

{

    return +[](void * uptr, const tdsl::tds_colmetadata_token & colmd,

               const tdsl::tdsl_row & row) -> void {

        SqlCatalogRowCallbackImpl(uptr, colmd, row);

    };

}



tdsl::netburner_driver::sql_command_row_callback SqlDatabaseCatalogRowCallback()

{

    return +[](void * uptr, const tdsl::tds_colmetadata_token & colmd,

               const tdsl::tdsl_row & row) -> void {

        SqlDatabaseCatalogRowCallbackImpl(uptr, colmd, row);

    };

}

tdsl::netburner_driver::sql_command_row_callback SqlColumnCatalogRowCallback()
{
    return +[](void * uptr, const tdsl::tds_colmetadata_token & colmd,
               const tdsl::tdsl_row & row) -> void {
        SqlColumnCatalogRowCallbackImpl(uptr, colmd, row);
    };
}

