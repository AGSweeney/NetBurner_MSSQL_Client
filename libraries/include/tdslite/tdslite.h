// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include <tdslite/tdslite.hpp>
#include <tdslite/util/tdsl_endian_check.hpp>
#include <tdslite-net/base/network_io_base.hpp>
#include <tdslite-net/netburner/tdsl_netimpl_netburner.hpp>
namespace tdsl { using netburner_driver = driver<net::tdsl_netimpl_netburner>; }
