// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#ifndef TDSLITE_SQL_CATALOG_H

#define TDSLITE_SQL_CATALOG_H



#include <TDSLite/tdslite.h>



#define SQL_CATALOG_MAX_TABLES     64

#define SQL_CATALOG_MAX_DATABASES  64

#define SQL_CATALOG_MAX_COLUMNS    64

#define SQL_CATALOG_NAME_LEN       128

#define SQL_CATALOG_TYPE_LEN       32



struct sql_table_catalog {

    bool ready;

    bool success;

    char status[160];

    char database[64];

    tdsl::uint32_t table_count;

    char table_names[SQL_CATALOG_MAX_TABLES][SQL_CATALOG_NAME_LEN];

};



struct sql_database_catalog {

    bool ready;

    bool success;

    char status[160];

    tdsl::uint32_t database_count;

    char database_names[SQL_CATALOG_MAX_DATABASES][SQL_CATALOG_NAME_LEN];

};

struct sql_column_catalog {

    bool ready;

    bool success;

    char status[160];

    char database[64];

    char table[SQL_CATALOG_NAME_LEN];

    tdsl::uint32_t column_count;

    char column_names[SQL_CATALOG_MAX_COLUMNS][SQL_CATALOG_NAME_LEN];

    char column_types[SQL_CATALOG_MAX_COLUMNS][SQL_CATALOG_TYPE_LEN];

};



#define SQL_BROWSE_TABLES_QUERY "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME"
#define SQL_BROWSE_DATABASES_QUERY "SELECT [name] FROM sys.databases ORDER BY [name]"



void SqlCatalogReset();

sql_table_catalog & SqlCatalogGet();

void SqlCatalogInvalidateTables();

bool SqlCatalogTablesMatchDatabase(const char * database);

void SqlColumnCatalogReset();

sql_column_catalog & SqlColumnCatalogGet();

void SqlColumnCatalogInvalidate();

bool SqlColumnCatalogMatches(const char * database, const char * table);



void SqlDatabaseCatalogReset();

sql_database_catalog & SqlDatabaseCatalogGet();



tdsl::netburner_driver::sql_command_row_callback SqlCatalogRowCallback();

tdsl::netburner_driver::sql_command_row_callback SqlDatabaseCatalogRowCallback();

tdsl::netburner_driver::sql_command_row_callback SqlColumnCatalogRowCallback();



#endif

