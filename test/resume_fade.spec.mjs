// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * The audio coming back without a click.
 *
 * While the context is suspended (a phone's audio taken by a call or another app, the page hidden), the device hears
 * silence; whatever the engine was playing carries on from where it was when the audio comes back, mid-waveform, and
 * the step from silence into it is a click. So the worklet fades its output in after the audio has been away. Heard
 * here on the dummy guest's tone, through a tap on the engine's output: every block before the suspend at full level,
 * and the first after it rising from silence.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("the output fades in when the audio comes back, and nowhere else", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const ac = cw.audioContext;
    // a tap on the engine's output: every block, as rendered
    const src = `registerProcessor("tap", class extends AudioWorkletProcessor { process(ins) { const ch = ins[0]?.[0]; if (ch) this.port.postMessage(ch.slice()); return true; } });`;
    await ac.audioWorklet.addModule(URL.createObjectURL(new Blob([src], { type: "text/javascript" })));
    const tap = new AudioWorkletNode(ac, "tap", { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1] });
    const blocks = [];
    tap.port.onmessage = ({ data }) => blocks.push(data);
    const quiet = ac.createGain();
    quiet.gain.value = 0;   // heard by the test alone
    cw.node.connect(tap);
    tap.connect(quiet).connect(ac.destination);
    cw.send("/dummy/tone");
    const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
    await sleep(600);
    const peak = (d) => d.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
    const playing = blocks.slice(-40).map(peak);   // settled: the tone at its level, block after block
    await cw.suspend();
    await sleep(500);
    const before = blocks.length;
    await cw.resume();
    await sleep(400);
    const back = blocks.slice(before, before + 12);
    await cw.shutdown();
    // the first blocks after: from silence, rising, to the tone's level by the fade's end (20 ms: 8 blocks at 48 kHz)
    return { playing: { min: Math.min(...playing), max: Math.max(...playing) }, first: back[0]?.[0] ?? null, backPeaks: back.map(peak), rate: ac.sampleRate };
  }, clockworkConfig);
  expect(r.playing.min, "a block at a lower level mid-tone: a fade where the audio never went").toBeGreaterThan(r.playing.max * 0.95);
  expect(Math.abs(r.first), "the first sample back is not silence: the tone starts mid-waveform, a click").toBeLessThan(0.01);
  expect(r.backPeaks[0], "the first block back is at full level").toBeLessThan(r.playing.max * 0.5);
  expect(r.backPeaks.at(-1), "the fade never reaches the tone's level").toBeGreaterThan(r.playing.max * 0.95);
  for (let i = 1; i < 6; i++) expect(r.backPeaks[i], `block ${i} back is quieter than the one before`).toBeGreaterThanOrEqual(r.backPeaks[i - 1] * 0.98);
});
