// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * Fixtures for clockwork's own browser tests.
 *
 * `clockworkConfig` points every test at the artifacts build-web.sh produces and at
 * the transport the current project selects. Both transports run always: SAB
 * and postMessage differ in who can see the guest's heap, and that difference
 * is where clockwork's own defects have consistently lived.
 */
import { test as base } from "@playwright/test";

export const test = base.extend({
  /*
   * Declared as an OPTION so playwright.config can set it per project.
   * A plain fixture cannot be overridden from `use`, and Playwright refuses
   * the whole project rather than ignoring it.
   */
  clockworkMode: ["postMessage", { option: true }],

  clockworkConfig: async ({ clockworkMode }, use) => {
    const mode = clockworkMode;
    await use({
      mode,
      baseURL: "/dist/",
      wasmBaseURL: "/dist/wasm/",
      wasmUrl: "/dist/wasm/clockwork.wasm",
      workerBaseURL: "/dist/workers/",
      workletUrl: "/dist/workers/clockwork_audio_worklet.js",
      snapshotIntervalMs: 25,
    });
  },

});

export { expect } from "@playwright/test";

/** Navigate and wait for the client module to load. */
export async function boot(page) {
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  await page.goto("/test/self-test.html");
  await page.waitForFunction(() => window.clockworkReady === true, { timeout: 10000 });
  return errors;
}
