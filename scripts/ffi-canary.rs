// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The canary scripts/check-ffi.sh must catch: an extern "C" function whose
// body is not the panic fence alone. Never compiled.
#[no_mangle]
pub extern "C" fn clockwork_ffi_canary(x: u32) -> u32 {
    x.checked_add(1).unwrap()
}
