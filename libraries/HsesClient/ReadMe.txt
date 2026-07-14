HsesClient — NetBurner port of MotoHSES (Motoman HSES UDP client)

Protocol
  YERC framing over UDP port 10040 (robot division).
  File-control streaming (UDP 10041) is not in this port.

Enable in makefile (before boilerplate.mk):

  HSES_ROOT ?= /path/to/NetBurner_MSSQL_Client
  LIBS_HSES := 1
  include ../../libraries/HsesClient/library.mak

  (On Windows prefer the relative include so make rules have no drive-letter colons.)

Umbrella include:

  #include <HsesClient/hses_client.h>
  // or #include <HsesClient/moto_hses.h>

Public API (namespace moto::hses):
  Client::open / close / is_open / request
  readStatusAll, readPositionPulse / readPositionCartesian
  Byte/Int/Dint/Real/Char variables, I/O, registers
  Servo/hold/job/move helpers (same surface as MotoHSES host library,
  minus file-control streaming)

Provenance
  Derived from MotoHSES (MIT). See THIRD_PARTY_NOTICES.txt.

Full guide: docs/hses-integration.md
