# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
add_library(dummy STATIC dummy_dsp.cpp)
target_link_libraries(dummy PRIVATE clockwork)
