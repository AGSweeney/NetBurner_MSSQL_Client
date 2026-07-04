TDSLite Library for NetBurner
=============================

Lightweight Microsoft SQL Server (MSSQL) connector for NetBurner modules,
based on the open-source tdslite project (https://github.com/tdslite/tdslite).

Overview
--------

tdslite implements the MS-TDS (Tabular Data Stream) protocol in pure C++11
with zero external dependencies. This NetBurner port adds a TCP networking
adapter that uses the standard NetBurner socket API (connect, ReadAllWithTimeout,
writeall, GetHostByName).

The core protocol headers are vendored from upstream tdslite. The NetBurner-specific
network layer lives in:

  libraries/include/tdslite-net/netburner/tdsl_netimpl_netburner.hpp

Public API
----------

Include the umbrella header:

  #include <TDSLite/tdslite.h>

Main type:

  tdsl::netburner_driver driver{net_buf};

Build Integration
-----------------

Add to your project makefile BEFORE boilerplate.mk:

  LIBS_TDSLITE := 1
  TDSLITE_ROOT := /path/to/NetBurner_MSSQL_Client
  include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak
  include $(NNDK_ROOT)/make/boilerplate.mk

To install into the NetBurner SDK, copy:

  libraries/include/tdslite/
  libraries/include/tdslite-net/
  libraries/include/TDSLite/tdslite.h
  libraries/TDSLite/

into $(NNDK_ROOT)/libraries/ and $(NNDK_ROOT)/libraries/include/ respectively,
then set TDSLITE_ROOT := $(NNDK_ROOT).

Network Buffer
--------------

Allocate a static buffer of at least 768 bytes (512 minimum). The driver uses
this buffer for TDS send/receive I/O:

  tdsl::uint8_t net_buf[768] = {};
  tdsl::netburner_driver driver{net_buf};

Requirements
------------

- NetBurner module with Ethernet and TCP/IP stack initialized
- C++11 or later (NetBurner NNDK gnu++17 is supported)
- Microsoft SQL Server or compatible TDS endpoint (default port 1433)

Example
-------

See examples/NetBurner_MSSQL_Client for a web UI SQL query test application.

License
-------

tdslite core: MIT License (see LICENSE.md)
NetBurner adapter and packaging: MIT License
