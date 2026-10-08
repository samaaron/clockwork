// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_nif_front.h — where a product's front (OscFront.h) reaches the NIF.
 *
 * A native host installs a product's front itself. The NIF is a host clockwork
 * builds, so a product reaches it through this function instead: the NIF
 * calls it once per boot, with the engine it is booting and the door that
 * engine's replies leave by, sends every packet through what it returns, and
 * offers it every reply. SuperSonic's front reads the synthdef and sample
 * files the engine will not, as it does for SuperSonic's own server.
 *
 * Clockwork's build defines it to make none (clockwork_nif_no_front.cpp). A
 * product that has a front configures with CLOCKWORK_NIF_FRONT=ON and adds its
 * own definition to clockwork_nif.
 */
#pragma once

#include "OscFront.h"

#include <memory>

class ClockworkEngine;

std::unique_ptr<OscFront> clockwork_nif_make_front(ClockworkEngine& engine, IOscTransport& replies);
