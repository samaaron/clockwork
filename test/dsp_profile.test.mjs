// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * The client boundary: one client, either DSP's vocabulary.
 *
 *   node --test test/dsp_profile.test.mjs
 *
 * These are the two profiles that matter — clockwork's and scsynth's — plus the
 * refusals, because a profile that is wrong should fail where it is supplied
 * rather than at the first sync, which happens later and reads like a
 * transport bug.
 *
 * The profile got SMALLER on 2026-09-01. It used to declare a definition
 * verb, a name-reader for definition blobs, and two forget verbs, so the
 * client could keep a cache of definitions and replay it after a reload.
 * Every other kind of state a product held was already restored by
 * restoreClientState(); definitions had a mechanism to themselves for no
 * reason except that somebody had built one. What is left is the vocabulary a
 * client genuinely cannot work without: the barrier, and the verbs it must
 * refuse locally.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import { dspProfile, NO_DSP } from "../js/lib/dsp_profile.js";

const clockwork = () =>
  dspProfile({
    syncVerb: "/clockwork/sync",
    syncedVerb: "/clockwork/synced",
    blockedVerbs: { "/clockwork/quit": "Use destroy() to shut down." },
  });

const scsynth = () =>
  dspProfile({
    syncVerb: "/sync",
    syncedVerb: "/synced",
  });

test("each DSP brings its own vocabulary", () => {
  assert.equal(clockwork().syncVerb, "/clockwork/sync");
  assert.equal(scsynth().syncVerb, "/sync");
  assert.equal(clockwork().syncedVerb, "/clockwork/synced");
  assert.equal(scsynth().syncedVerb, "/synced");
});

test("blocked verbs are the DSP's business", () => {
  assert.deepEqual(Object.keys(clockwork().blockedVerbs), ["/clockwork/quit"]);
  assert.deepEqual(Object.keys(scsynth().blockedVerbs), []);
});

test("no profile is a working state, not an error", () => {
  const p = dspProfile(undefined);
  assert.equal(p, NO_DSP);
  assert.equal(p.syncVerb, null, "sync is unavailable rather than wrong");
  assert.deepEqual(Object.keys(p.blockedVerbs), []);
});

test("the profile no longer carries a definition format", () => {
  // Named explicitly rather than left to absence: these four fields were the
  // client's half of a definition cache, and a profile that still supplies
  // them should get a validated profile that ignores them, not one that
  // quietly resurrects the caching by carrying them through.
  const p = dspProfile({
    syncVerb: "/sync",
    syncedVerb: "/synced",
    defineVerb: "/d_recv",
    nameOf: () => "a-synthdef",
    forgetVerb: "/d_free",
    forgetAllVerb: "/d_freeAll",
  });
  assert.equal(p.defineVerb, undefined);
  assert.equal(p.nameOf, undefined);
  assert.equal(p.forgetVerb, undefined);
  assert.equal(p.forgetAllVerb, undefined);
});

test("a malformed profile fails where it is supplied", () => {
  assert.throws(() => dspProfile({ syncVerb: "sync", syncedVerb: "/synced" }), /OSC address/);
  // A half-declared barrier would wait forever on a reply nobody sends.
  assert.throws(() => dspProfile({ syncVerb: "/sync" }), /come as a pair/);
  assert.throws(() => dspProfile({ syncedVerb: "/synced" }), /come as a pair/);
});

test("a profile cannot be mutated after validation", () => {
  const p = clockwork();
  assert.throws(() => {
    "use strict";
    p.syncVerb = "/something/else";
  });
  assert.equal(p.syncVerb, "/clockwork/sync");
});
