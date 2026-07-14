// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney

#include <micro800/cip_types.hpp>

namespace micro800 {

const char *CipTypeName(uint16_t typeCode)
{
    switch (typeCode) {
    case 0xC1:
        return "BOOL";
    case 0xC2:
        return "SINT";
    case 0xC3:
        return "INT";
    case 0xC4:
        return "DINT";
    case 0xC5:
        return "LINT";
    case 0xC6:
        return "USINT";
    case 0xC7:
        return "UINT";
    case 0xC8:
        return "UDINT";
    case 0xC9:
        return "ULINT";
    case 0xCA:
        return "REAL";
    case 0xCB:
        return "LREAL";
    case 0xCC:
        return "TIME";
    case 0xCD:
        return "DATE";
    case 0xCE:
        return "TIME_OF_DAY";
    case 0xCF:
        return "DATE_AND_TIME";
    case 0xD1:
        return "BYTE";
    case 0xD2:
        return "WORD";
    case 0xD3:
        return "DWORD";
    case 0xD4:
        return "LWORD";
    case 0xDA:
        return "STRING";
    default:
        return "";
    }
}

} // namespace micro800
