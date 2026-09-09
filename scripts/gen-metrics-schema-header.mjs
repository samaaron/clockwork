#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//
// Generates src/metrics_schema.h and rust/clockwork-client/src/metrics_schema.rs
// from js/lib/metrics_schema.js so native GUIs (a Qt metrics panel,
// clockwork-state, a Rust host on clockwork-client, ...) can render the same
// metric names and descriptions as the <clockwork-metrics> web component.
//
// Usage:  npm run gen:metrics-header
// Commit the regenerated files alongside changes to metrics_schema.js;
// test/metrics_schema.test.mjs fails while they are out of step.

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { METRICS_SCHEMA } from '../js/lib/metrics_schema.js';

const cEscape = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');

// Render the C++ header text from the schema. Exported so the staleness
// test can compare against the committed src/metrics_schema.h.
export function renderHeader(schema = METRICS_SCHEMA) {
  const metrics = Object.entries(schema.metrics)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.offset - b.offset);

  const nativeStats = Object.entries(schema.nativeStats)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.index - b.index);

  const composites = Object.entries(schema.composites)
    .map(([key, def]) => ({ key, ...def }));

  const compositeLines = composites.map((m) =>
    `    { "${cEscape(m.key)}", "${cEscape(m.description)}" },`);

  const fieldLines = metrics.map((m) =>
    `    { ${String(m.offset).padStart(2)}, "${cEscape(m.key)}", "${cEscape(m.unit ?? '')}", "${cEscape(m.description)}" },`);

  const nativeLines = nativeStats.map((m) =>
    `    { ${m.index}, "${cEscape(m.key)}", "${cEscape(m.unit ?? '')}", "${cEscape(m.description)}" },`);

  return `// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// Nothing here checks it is in step with the schema; regenerate after any
// change to metrics_schema.js.
//
// Metric names, units and human-readable descriptions for the clockwork
// performance metrics, for use by native GUIs. Offsets index the
// PerformanceMetrics struct (see shared_memory.h); native-stat indices
// address the separate NATIVE_STATS segment.

#pragma once

#include <cstdint>
#include <cstring>

namespace clockwork {
namespace metrics_schema {

struct FieldInfo
{
    uint32_t offset;         // index into the metrics Uint32 array
    const char* key;         // schema key (stable identifier)
    const char* unit;        // unit of measurement ("" if none)
    const char* description; // human-readable description
};

inline constexpr FieldInfo kFields[] = {
${fieldLines.join('\n')}
};

struct NativeStatInfo
{
    uint32_t index;          // u32 slot within the NATIVE_STATS segment
    const char* key;
    const char* unit;
    const char* description;
};

inline constexpr NativeStatInfo kNativeStats[] = {
${nativeLines.join('\n')}
};

// Rows combining several metrics in one reading ("current | peak", ...).
struct CompositeInfo
{
    const char* key;
    const char* description;
};

inline constexpr CompositeInfo kComposites[] = {
${compositeLines.join('\n')}
};

inline const char* descriptionForComposite(const char* key)
{
    for (const CompositeInfo& c : kComposites)
        if (std::strcmp(c.key, key) == 0)
            return c.description;
    return nullptr;
}

inline const char* descriptionForOffset(uint32_t offset)
{
    for (const FieldInfo& f : kFields)
        if (f.offset == offset)
            return f.description;
    return nullptr;
}

inline const char* descriptionForNativeStat(uint32_t index)
{
    for (const NativeStatInfo& f : kNativeStats)
        if (f.index == index)
            return f.description;
    return nullptr;
}

} // namespace metrics_schema
} // namespace clockwork
`;
}

// The Rust twin: the same three tables for clockwork-client, plus one enum
// per indexed table so a reading is asked for by name (`Metric::AudioSampleRate`)
// and a typo is a compile error rather than a wrong slot. Variant names are
// the schema keys in PascalCase; the discriminant is the offset or index.
const rustEscape = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
const pascal = (key) => key.charAt(0).toUpperCase() + key.slice(1);

export function renderRust(schema = METRICS_SCHEMA) {
  const metrics = Object.entries(schema.metrics)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.offset - b.offset);

  const nativeStats = Object.entries(schema.nativeStats)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.index - b.index);

  const composites = Object.entries(schema.composites)
    .map(([key, def]) => ({ key, ...def }));

  const fieldLines = metrics.map((m) =>
    `    FieldInfo { offset: ${m.offset}, key: "${rustEscape(m.key)}", unit: "${rustEscape(m.unit ?? '')}", description: "${rustEscape(m.description)}" },`);
  const nativeLines = nativeStats.map((m) =>
    `    NativeStatInfo { index: ${m.index}, key: "${rustEscape(m.key)}", unit: "${rustEscape(m.unit ?? '')}", description: "${rustEscape(m.description)}" },`);
  const compositeLines = composites.map((m) =>
    `    CompositeInfo { key: "${rustEscape(m.key)}", description: "${rustEscape(m.description)}" },`);

  const metricVariants = metrics.map((m) => `    ${pascal(m.key)} = ${m.offset},`);
  const metricAll = metrics.map((m) => `Metric::${pascal(m.key)}`);
  const metricInfo = metrics.map((m, i) => `            Metric::${pascal(m.key)} => &FIELDS[${i}],`);
  const statVariants = nativeStats.map((m) => `    ${pascal(m.key)} = ${m.index},`);
  const statAll = nativeStats.map((m) => `NativeStat::${pascal(m.key)}`);
  const statInfo = nativeStats.map((m, i) => `            NativeStat::${pascal(m.key)} => &NATIVE_STATS[${i}],`);

  return `// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// test/metrics_schema.test.mjs fails when this file is out of step with the
// schema, as it does for the C++ twin, src/metrics_schema.h.
//
//! The metrics schema, by name.
//!
//! The engine's counters are a flat \`u32\` array a client reads as a block
//! ([\`crate::Metrics\`]); the device layer's own account of its callback is a
//! second, smaller one ([\`crate::NativeStats\`]). This module says what each
//! word means — the key, unit and description every GUI shows, the same words
//! the web component and a Qt panel use — and gives each table an enum so a
//! reading is asked for as \`Metric::AudioSampleRate\` rather than as word 42.
//!
//! Offsets are not dense: the schema numbers context metrics the web host
//! merges in after the engine's, and a native engine reports fewer words than
//! the highest offset here. \`Metrics::get\` answers \`None\` for a word the
//! engine did not report.

/// One performance metric: \`offset\` indexes the metrics \`u32\` array.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FieldInfo {
    pub offset: u32,
    /// The schema key: the stable identifier, shared with the web UI.
    pub key: &'static str,
    /// The unit of measurement; empty for a state.
    pub unit: &'static str,
    pub description: &'static str,
}

/// One native stat: \`index\` is the \`u32\` slot within the NATIVE_STATS region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NativeStatInfo {
    pub index: u32,
    pub key: &'static str,
    pub unit: &'static str,
    pub description: &'static str,
}

/// A row combining several metrics in one reading ("current | peak", ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CompositeInfo {
    pub key: &'static str,
    pub description: &'static str,
}

/// Every performance metric, in offset order.
pub const FIELDS: &[FieldInfo] = &[
${fieldLines.join('\n')}
];

/// Every native stat, in index order.
pub const NATIVE_STATS: &[NativeStatInfo] = &[
${nativeLines.join('\n')}
];

/// The composite rows.
pub const COMPOSITES: &[CompositeInfo] = &[
${compositeLines.join('\n')}
];

/// The performance metrics by name. \`m as u32\` is the offset.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Metric {
${metricVariants.join('\n')}
}

impl Metric {
    /// Every metric, in offset order.
    pub const ALL: &'static [Metric] = &[${metricAll.join(', ')}];

    /// The word this metric is, in the metrics array.
    pub const fn offset(self) -> u32 {
        self as u32
    }

    /// Key, unit and description.
    pub const fn info(self) -> &'static FieldInfo {
        match self {
${metricInfo.join('\n')}
        }
    }

    /// The schema key.
    pub const fn key(self) -> &'static str {
        self.info().key
    }
}

/// The native stats by name. \`s as u32\` is the slot.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NativeStat {
${statVariants.join('\n')}
}

impl NativeStat {
    /// Every native stat, in index order.
    pub const ALL: &'static [NativeStat] = &[${statAll.join(', ')}];

    /// The slot this stat is, in the NATIVE_STATS region.
    pub const fn index(self) -> u32 {
        self as u32
    }

    /// Key, unit and description.
    pub const fn info(self) -> &'static NativeStatInfo {
        match self {
${statInfo.join('\n')}
        }
    }

    /// The schema key.
    pub const fn key(self) -> &'static str {
        self.info().key
    }
}
`;
}

// CLI entry: write the header and the Rust module in place.
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const root = join(dirname(fileURLToPath(import.meta.url)), '..');
  const header = join(root, 'src', 'metrics_schema.h');
  writeFileSync(header, renderHeader());
  console.log(`wrote ${header}`);
  const rust = join(root, 'rust', 'clockwork-client', 'src', 'metrics_schema.rs');
  writeFileSync(rust, renderRust());
  console.log(`wrote ${rust}`);
}
