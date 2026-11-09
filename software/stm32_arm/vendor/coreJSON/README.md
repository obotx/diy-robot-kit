# coreJSON v3.3.1

Unmodified FreeRTOS coreJSON, MIT licensed. No RTOS or heap is required.

Upstream: https://github.com/FreeRTOS/coreJSON/tree/v3.3.1

Downloaded from the pinned tag on 2026-09-08:

| Local file | Upstream path | SHA-256 |
| --- | --- | --- |
| core_json.h | source/include/core_json.h | 4289d6f8f5d1360fdde122f475b1fbf73e27d780be1f11a32212e873b4d0c59b |
| core_json.c | source/core_json.c | 3d1a7a4e5e379586f9f25594c03b5cfab97775a6f663f2dbec85df3bf61357a0 |
| LICENSE | LICENSE | 508a77d2e7b51d98adeed32648ad124b7b30241a8e70b2e72c99f92d8e5874d1 |

arm_link.c calls JSON_Validate before iteration, then enforces exact schemas,
rejects duplicates/escaped keys, validates numeric types, ranges and sessions.
