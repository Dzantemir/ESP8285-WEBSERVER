#ifndef SMART_AP_COMMON_H
#define SMART_AP_COMMON_H

// ============================================================================
// CONVENIENCE HEADER — includes all project sub-headers.
//
// Architecture (Bug #10 fix — no more god header):
//   smart_ap_config.h  — Kconfig values, derived constants, compile guards
//   smart_ap_types.h   — enums, structs, sys_ctx_t, CTX_CHECK macros
//   smart_ap_api.h     — public function prototypes
//
// Each .c file includes smart_ap_common.h PLUS only the system headers
// it actually needs — no more pulling in every ESP-IDF header everywhere.
// ============================================================================

#include "smart_ap_types.h"
#include "smart_ap_config.h"
#include "smart_ap_api.h"

#endif // SMART_AP_COMMON_H
