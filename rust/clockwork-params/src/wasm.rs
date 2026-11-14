// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The conversions for the web, on the same Rust as every other host (the page's
//! glue is js/lib/params_manager.js). An entry comes as its bytes and its run, as
//! js/lib/params.js read them from the region.
#![cfg(target_arch = "wasm32")]

use wasm_bindgen::prelude::*;

use crate::reader::Param;

fn param(entry: &[u8], text: &[u8]) -> Result<Param, JsError> {
    Param::from_bytes(entry, text).ok_or_else(|| JsError::new("not a parameter entry"))
}

#[wasm_bindgen]
pub fn param_position_of(entry: &[u8], value: f32) -> Result<f32, JsError> {
    Ok(param(entry, &[])?.position_of(value))
}

#[wasm_bindgen]
pub fn param_value_at(entry: &[u8], position: f32) -> Result<f32, JsError> {
    Ok(param(entry, &[])?.value_at(position))
}

#[wasm_bindgen]
pub fn param_text_of(entry: &[u8], text: &[u8], value: f32) -> Result<String, JsError> {
    Ok(param(entry, text)?.text_of(value))
}

/// The value the words mean, or undefined when they mean none.
#[wasm_bindgen]
pub fn param_value_of_text(entry: &[u8], text: &[u8], words: &str) -> Result<Option<f32>, JsError> {
    Ok(param(entry, text)?.value_of_text(words))
}
