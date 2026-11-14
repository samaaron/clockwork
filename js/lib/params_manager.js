// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * params_manager.js — what a parameter's value means, on the web: its place on a
 * knob and its words, from the same Rust every other host calls (clockwork-params,
 * compiled by build-web-subsystems.sh), so a page shows what a plugin host shows.
 *
 * Reading the table needs none of this: params.js parses the memory on its own.
 * `entry` here is one that reader returned, or ParamsReader's.
 */
import init, {
  param_position_of,
  param_text_of,
  param_value_at,
  param_value_of_text,
} from "../../dist/params/clockwork_params.js";

export class ParamConversions {
  /** @param {object} [options] @param {string|URL|Uint8Array} [options.wasm] where the core's wasm is */
  constructor(options = {}) { this._wasm = options.wasm; }

  async init() {
    await init(this._wasm !== undefined ? { module_or_path: this._wasm } : undefined);
    return this;
  }

  /** Where a value sits on a knob, 0 to 1. */
  positionOf(entry, value) { return param_position_of(entry.bytes, value); }
  /** The value at a place on a knob, as the table would hold it. */
  valueAt(entry, position) { return param_value_at(entry.bytes, position); }
  /** A value in words. */
  textOf(entry, value) { return param_text_of(entry.bytes, entry.textBytes, value); }
  /** What someone typed, as a value; undefined when the words mean none. */
  valueOfText(entry, words) { return param_value_of_text(entry.bytes, entry.textBytes, words); }
}
