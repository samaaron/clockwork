<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Devices

How a native engine comes to play on an audio device, and onto which one.

**Built 2026-10-17.** Before this, the engine drove JUCE's
`AudioDeviceManager`, which opened the system defaults and was then told to
change them. Every way onto a device went through the defaults first. Now
the engine decides what to open and Smoothie opens exactly that.

## Two parts

| part | where | decides | does |
|---|---|---|---|
| **the device session** | `smoothie/worklet/smoothie_devices.h` | nothing | lists the drivers and their devices; opens what it is asked to; runs the processor on it |
| **the engine's device paths** | `src/native/ClockworkEngine.cpp` | which device, input, rate and buffer, and when | asks the session, then restarts around the device (docs/RESTARTS.md) |

The web and an embedded host have no session. On the web the browser owns
the device, and an embedder that pumps the engine owns its own. Each of them
hands the engine the same facts (rate, buffer, channels, latency), through
the worklet's `Processor` (`smoothie_worklet.h`) on native and through the
AudioWorklet on the web. A change to those facts is the same hot or cold
restart on every target.

## The session

`smoothie::DeviceSession` holds no JUCE in its header. Its drivers are
JUCE's device types (`smoothie_juce_drivers.h`):

- **the platform's:** every driver JUCE knows, in the order its device
  manager listed them. A platform or build gets the ones it has: WASAPI's
  three modes, DirectSound and ASIO; CoreAudio; iOS; Bela, ALSA and JACK;
  Oboe, OpenSL and Android's Java audio.
- **or a factory's:** Clockwork's Linux build adds its PipeWire driver
  (`platformDriversWithPipeWire`), and tests hand in fakes.

A session is made once per engine, and again by every recovery. Only a new
session gets a new connection to the OS's audio service (a hibernate-killed
CoreAudio client is not revived by reopening).

**Opening.** A `DeviceRequest` names a driver, an output, an input, a rate,
a buffer and channel counts, and nothing is filled in that the caller did
not name:

- An open that fails leaves nothing open, the device before it included, and
  says why.
- A request equal to what the open device settled on leaves it alone.
- One that keeps the devices but changes the rate, buffer or channels opens
  the same device again.
- One that changes the devices closes the old one first.
- Moving to another driver with a device open waits `driverSettleMs` (1.5 s)
  between the close and the open, so DirectSound and ASIO on one interface
  don't clash.
- A rate the device doesn't offer gives way to the rate it runs at now, then
  to the lowest it offers from 44.1 kHz up. A buffer it doesn't offer gives
  way to its default. These are JUCE's manager's rules, kept as they were.

**Running.** An open device runs from the moment it opens, silent until a
processor is attached. Attached, the processor is told the device is
starting, and stays attached through whatever the session opens and closes
after, told of each stop and start. `detach` returns only once no block is
inside the processor. The audio thread takes no lock for that.

**Asking.** The session also answers what the engine asks of the drivers:
- device names and the OS's default device;
- what a driver knows of a device without opening it (wireless, virtual,
  aggregate, whether it pairs with another device);
- a probe, which creates a device to read its rates and channels without
  opening it;
- the open device's live state.

**Changes.** A driver reports a change on whatever thread the OS tells it
on: the list changed, the default moved, or the open device changed. The
session passes the note on and does nothing else. The engine queues one pass
of `reconcileDevices` on its device lane, which looks and decides.

## What the engine opens

| when | what is opened |
|---|---|
| **boot** | `-H`'s device (scoped to `--audio-driver` when one is given), else the system default — a wired one on macOS in place of a wireless default. With it, the user's input when that input pairs with the output; else the default input, when inputs are wanted and `-H` named no device. All at the session's rate and buffer, in one open. What won't open gives way a part at a time: the input, then `-H`'s device, then the default's input, then the requested driver. |
| **a switch** (`/clockwork/devices/switch`, `switchDevice`) | the named device on its driver, with the input asked for or the one already playing. Cold when the rate, channels or driver change, unless the guest survives that change. A device that won't open, or opens and plays nothing for 5 s, is backed out: the engine goes back to what played before. |
| **system mode** (`__system__`, `setDeviceMode("")`) | the OS's default. A wired one is opened by name, like any switch. A wireless one, or one on a driver with no default (ASIO), is opened by its driver's own default route. |
| **the default moves** | the new default, in system mode only. Not a virtual device another app made the default, and never away from a device the user chose. |
| **a device comes back** | the user's output or input, when it reappears in the list. |
| **a recovery** (the watchdog's stall or rate-skew verdict, `/clockwork/devices/reopen`, the playing device gone) | a fresh session, then the user's output if it is listed, else the system default, with the user's input. The chosen device is opened directly, never the default first. Always cold, so clients rebuild (`setup`). A chosen device that won't play gives way to the default. |
| **a reopen in place** (the device's channels changed, or its driver says it must be reopened) | the device playing now, cold. |

Every open is heard as a click, a dropout, or a moment of silence on
whatever else plays through the device. On CoreAudio an aggregate is also
made and pulled down. `test/test_device_intent.cpp` pins the boot and
recovery opens to one each.

The engine's own sound dips rather than clicks. A restart fades what the
device plays down to silence over 20 ms before the device stops, and up
from silence over 20 ms once audio resumes: the web's resume fade
(`js/workers/clockwork_audio_worklet.js`), natively. The restart waits for
the fade-out while the device goes on calling; a device that has stopped
calling has nothing left to fade. The scope and the taps carry the output
unfaded. `test/test_restart_fade.cpp` listens across hot and cold restarts.

## What the engine remembers

The user's choices outlive the device they were made on: a pinned output is
played on again when it is plugged back in.

| memory | what it is | set by |
|---|---|---|
| pinned output (`mPreferredOutputDevice`) | the output to play on whenever it is there; empty means follow the system default | `-H`, a user switch, `setDeviceMode` |
| `-H`'s words (`mPreferredOutputIsWords`) | the pin is still `-H`'s text and not yet a device's name. It is matched as `-H` matches when the list changes | boot, until a device matches |
| device mode (`mDeviceMode`) | the selection as reported: a device name, or empty for system | boot, `setDeviceMode` |
| preferred input (`mPreferredInputDevice`) | the input to record from whenever it is there | `-H`'s input, a user switch |
| last input (`mLastInputDeviceName`) | the input played with last, for turning inputs back on | a switch that names an input, boot opening the preferred one, inputs turned off or dropped for an output they won't pair with |
| input width (`mBootInputChannels`) | how many inputs to open (`-i`) | boot, `setConfiguredInputChannels` |
| boot driver (`mBootDriver`) | the driver boot settled on, which a recovery's fresh session goes back to | boot |
| pending driver (`mIntendedDriver`) | a driver the user picked that has no device opened yet | `switchDriver` |
| device per driver (`mPreferredDeviceByDriver`) | the device last opened on each driver, which a driver pick opens | every user switch |
| rate per device (`mDeviceRateMemory`), pre-wireless rate | rates to return to | every switch |
| refused pairs (`mUngatableInputPairs`) | an (output, input) pair whose open failed only with the input | a switch's input fallback |

Recovery and a reopen choose the input in one order: the preferred input if
it is listed, else the last input if it is listed.

## Threads and holds

- **The device lane** (`mDeviceTaskThread`, in the apartment its drivers
  need, `DeviceLaneApartment.h`) does every open, switch, recovery, reconcile
  pass and close, one at a time.
- **The swap gate** (`mSwapMutex`) is held across each device change. It is
  recursive: a recovery holds it through the switch it runs. Readers
  (`listDevices`, `currentDevice`, the device report) take it briefly.
- **The audio thread** reaches the session only through the attached
  processor, and takes no lock.

## Testing

No test opens a real device unless it is tagged `[.hardware]`
(`test/RealDeviceGuard.h`): a case that would reach the platform's drivers
fails by name. Cases run on the fake drivers (`test/FakeAudioDevice.h`): a
real session whose only drivers are fakes, with their devices scripted —

- **failing to open or to start;**
- **dying where they stand;**
- **lying about their rate;**
- **held at another rate by another app;**
- **unplugged and back.**

Each fake driver behaves like a real one (`behaveLike`): how CoreAudio,
WASAPI, ASIO, ALSA, JACK or PipeWire hears of a change, and whether it has a
default. `test/test_device_session.cpp` pins the session itself.
