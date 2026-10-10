// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A port's positions: how far its reader and its writer have got, in frames
//! since it opened. What lies between them is their difference, and the
//! reader's position taken with a time is what dates every frame behind it,
//! so they must move by exactly what was read and written — an underrun's
//! silence moving neither — and stay right across the ring's wrap.

use std::ffi::CString;

use clockwork_ports::ffi::*;

const SOURCE: i32 = 1;

#[test]
fn positions_move_by_what_was_read_and_written_and_nothing_else() {
    let name = CString::new("positions").unwrap();
    // SAFETY: a NUL-terminated CString.
    let port = unsafe { clockwork_port_open(name.as_ptr(), SOURCE, 2, 64) };
    assert_ne!(port, 0);
    assert_eq!(clockwork_port_read_position(port), 0);
    assert_eq!(clockwork_port_write_position(port), 0);

    let frames = vec![0.5f32; 64 * 2];
    let mut l = vec![0.0f32; 64];
    let mut r = vec![0.0f32; 64];
    let out: [*mut f32; 2] = [l.as_mut_ptr(), r.as_mut_ptr()];
    let (mut written, mut read) = (0u32, 0u32);
    // Many times round the ring, reading more than is there every third time.
    for i in 0..1000u32 {
        let offer = 1 + (i * 37) % 48;
        // SAFETY: `frames` holds 64 frames of 2 channels; `offer` is fewer.
        written += unsafe { clockwork_port_produce(port, frames.as_ptr(), offer) };
        let ask = if i % 3 == 0 { 64 } else { (i * 11) % 40 };
        let there = clockwork_port_readable(port);
        // SAFETY: two channel pointers to 64 writable frames; `ask` is at most 64.
        unsafe { clockwork_port_read(port, out.as_ptr(), 2, ask) };
        read += ask.min(there);

        assert_eq!(clockwork_port_write_position(port), written, "after write {i}");
        assert_eq!(clockwork_port_read_position(port), read, "after read {i}");
        assert_eq!(
            clockwork_port_write_position(port).wrapping_sub(clockwork_port_read_position(port)),
            clockwork_port_readable(port),
            "between them, after {i}"
        );
    }
    assert!(clockwork_port_underruns(port) > 0, "the underrun path was never taken");
    clockwork_port_close(port);
    assert_eq!(clockwork_port_read_position(port), 0, "a closed port has no positions");
}
