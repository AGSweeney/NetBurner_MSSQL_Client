Micro800Client — optional EtherNet/IP / CIP client for Allen-Bradley Micro800

Provenance
  Working protocol code extracted from NetBurnerGateway (CIP Data Gateway).
  REAJet / print-mapping code is intentionally NOT included.

Enable in makefile (before boilerplate.mk):

  MICRO800_ROOT ?= /path/to/NetBurner_MSSQL_Client
  LIBS_MICRO800 := 1
  include ../../libraries/Micro800Client/library.mak

  (On Windows prefer the relative include so make rules have no drive-letter colons.)

Umbrella include:

  #include <Micro800Client/micro800_client.h>

Public API (namespace micro800):
  ScanListIdentity, LooksLikeMicro800, ProbePlc
  ReadTag / ReadTagRaw, WriteTag, BrowseTags
  EncodeCipTagPath, CipTypeName, FormatTagValueText

Protocol:
  ENIP TCP 44818, ListIdentity UDP, CIP Read 0x4C / Write 0x4D / List Tags 0x55
  Each tag I/O call opens a new ENIP session (same model as Gateway).

Full guide: docs/micro800-integration.md
