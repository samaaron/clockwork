// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
use clockwork_osc::{OscArg, OscBundle, OscPacket};
fn main() {
    // A bundle shaped like the ones the scheduler delivers: timetag + messages.
    let inner = OscPacket::Bundle(OscBundle {
        timetag: 0x0000_0001_0000_0000,
        elements: vec![
            OscPacket::Message(clockwork_osc::OscMessage {
                addr: "/clockwork/sync".into(),
                args: vec![OscArg::Int(42)],
            }),
        ],
    });
    let bytes = clockwork_osc::encode_packet(&inner);
    println!("encoded {} bytes, starts {:?}", bytes.len(), std::str::from_utf8(&bytes[..8]));
    match clockwork_osc::decode_packet(&bytes) {
        Some(OscPacket::Bundle(b)) => println!("decoded bundle with {} elements", b.elements.len()),
        Some(OscPacket::Message(m)) => println!("decoded message {}", m.addr),
        None => println!("DECODE FAILED"),
    }
}
