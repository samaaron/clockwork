/*
 * Clockwork testing ITSELF in a browser.
 *
 * Until 2026-08-31 this did not exist. Clockwork had 9,499 lines of JS —
 * the client, the worklet, the transports, the managers — and two unit tests
 * over pure functions, with no page, no server and no config. Nothing it
 * owned could construct its own client, so nothing it owned could notice
 * that `cell_pool.js` had never been written, that `midi_manager.js` was a
 * syntax error, that the client asked the worklet for `copyCellData` while
 * the worklet only answered `copyBufferData`, or that its scheduler accepted
 * nothing at all in wasm. Every one of those was found by a CONSUMER.
 *
 * The device under test is dsp/dummy: it implements the whole boundary, needs no
 * engine, and is what build-web.sh already builds by default. A guest that
 * does nothing is exactly the right guest for testing the host.
 */
import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test",
  testMatch: "**/*.spec.mjs",
  timeout: 30000,
  retries: 0,
  workers: "50%",

  use: {
    baseURL: "http://localhost:8004",
    headless: true,
    launchOptions: {
      args: [
        "--use-fake-ui-for-media-stream",
        "--use-fake-device-for-media-stream",
        "--autoplay-policy=no-user-gesture-required",
      ],
    },
  },

  // Both transports, always. The two differ in who can see the guest's heap,
  // which is precisely where clockwork's own bugs have lived.
  projects: [
    { name: "SAB",         use: { browserName: "chromium", clockworkMode: "sab" } },
    { name: "postMessage", use: { browserName: "chromium", clockworkMode: "postMessage" } },
  ],

  webServer: {
    command: "node test/server.mjs",
    port: 8004,
    reuseExistingServer: false,
    timeout: 30000,
  },
});
