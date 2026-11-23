// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
use clockwork_osc::wire::{BundleWriter, Packet};
use clockwork_osc::Timetag;
fn main() {
    // A bundle shaped like the ones the scheduler delivers: timetag + messages.
    let mut buf = [0u8; 64];
    let mut bundle = BundleWriter::new(&mut buf, Timetag::from_ntp(0x0000_0001_0000_0000)).expect("room for a bundle");
    bundle.message("/clockwork/sync", |w| w.int(42).map(drop)).expect("room for a message");
    let bytes = bundle.finish();
    println!("encoded {} bytes, starts {:?}", bytes.len(), std::str::from_utf8(&bytes[..8]));
    match Packet::parse(bytes) {
        Ok(Packet::Bundle(b)) => println!("decoded bundle with {} elements", b.elements().count()),
        Ok(Packet::Message(m)) => println!("decoded message {}", m.address()),
        Err(why) => println!("DECODE FAILED: {why:?}"),
    }
}
