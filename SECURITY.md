<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# Security

## Reporting a vulnerability

Report it privately, through GitHub:
<https://github.com/samaaron/clockwork/security/advisories/new>,
or by email to <security@sonic-pi.net>.

Please do not open a public issue, pull request or discussion for a
vulnerability.

A useful report says:

- what is affected: the component, the commit or version, and the platform
- what an attacker needs: network reach to a port, a crafted sample file, a
  loaded plugin, a local account
- how to reproduce it, ideally with the smallest input that does
- whether you know of it being **actively exploited**. If so, put that in the
  title, and it goes to the front of the queue.

## What happens next

Clockwork handles vulnerabilities as the EU Cyber Resilience Act (Regulation
(EU) 2024/2847, Annex I Part II) requires of a manufacturer, and promises
nothing beyond that:

- Your report is acknowledged, and you are told who is handling it.
- A report is assessed, and a confirmed vulnerability is fixed without undue
  delay. The fix lands on `main` as a security update, on its own rather than
  inside other changes where that is practical, and free of charge.
- Disclosure is coordinated: you tell us before you publish, and we tell you
  before we do. When the fix is out, a GitHub Security Advisory with a CVE ID
  (GitHub is a CVE Numbering Authority) says what the vulnerability was, what
  it affects, how severe it is and what to do. In a justified case publication
  waits until users have had the chance to apply the fix. The reporter is
  credited unless they ask otherwise.
- A report of active exploitation goes to the front of the queue. A product
  that ships clockwork in the EU must give ENISA an early warning within 24
  hours of becoming aware of an actively exploited vulnerability, a
  notification within 72 hours and a final report within 14 days of a fix, and
  the products this project maintains (SuperSonic, Sonic Pi) report on that
  schedule.

There is no bug bounty.

## Supported versions

Clockwork has no tagged releases yet. Security fixes land on `main`, and the
projects that embed clockwork (SuperSonic, Sonic Pi) pin a commit and move it
forward to take them. Each advisory names the first fixed commit.

Once releases are tagged, this section will say which of them receive security
fixes and for how long: the support period of a release, and the date it ends,
are stated with the release.

## Shipping clockwork in a product

If you ship clockwork, you can be told about a vulnerability before it is
public: open a private advisory saying which product and who to contact, and
you will be added as a collaborator on advisories that affect what you ship.

What clockwork contains, and where each part came from, is in
[docs/components.cdx.json](docs/components.cdx.json). Every push, and every
day, CI writes a CycloneDX SBOM for each build target from that list and the
lockfiles and looks every component up for advisories; the same documents
can be regenerated from any commit with `scripts/sbom.mjs`. See
[docs/THREATS.md](docs/THREATS.md#supply-chain).

## How it is tested

Every parser that reads bytes from outside the process has a fuzz target.
On every push, each target is replayed over its corpus, the seeds and every
crasher ever found, deterministically, and the sample-file path is tried on
every sample Sonic Pi ships against ffmpeg. Every night, each target runs
under libFuzzer for two hours; a crasher is sealed to a key only the
maintainer holds before it leaves the runner, and once fixed joins the corpus.
`.github/workflows/security.yml` is the whole of it.

## Scope

[docs/THREATS.md](docs/THREATS.md) says what clockwork defends against and what
it trusts by design. In short:

- **In scope:** anything that lets bytes from a client, a network peer, a
  sample file or a MIDI device crash the process, read or write out of
  bounds, run code, or reach the file system beyond what the request names.
- **Out of scope:** a guest DSP or a loaded plugin doing what native code can
  do (they are trusted code by design); a client on the command transport
  controlling the engine (that is what the transport is for: protect the port);
  a denial of service that needs the attacker to already be such a client.
