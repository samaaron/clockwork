// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//
// The web module has no main: the AudioWorklet instantiates it and calls its
// exports (CLOCKWORK_WEB_EXPORTS in CMakeLists.txt). CMake wants one source
// behind a link target, and this is it.
