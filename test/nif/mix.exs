# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# The BEAM NIF, exercised from Elixir. Build the library first:
#
#   cmake -B build-nif -DCLOCKWORK_NIF=ON -DBUILD_TESTS=OFF .
#   cmake --build build-nif --target clockwork_nif
#   cd test/nif && CLOCKWORK_NIF_PATH=../../build-nif mix test
#
# CLOCKWORK_HEADLESS=1 boots the engine without an audio device (CI, or a
# machine whose device you would rather not open); CLOCKWORK_QUIET=1 silences
# the per-boot lifecycle lines.
defmodule ClockworkNifTest.MixProject do
  use Mix.Project

  def project do
    [
      app: :clockwork_nif_test,
      version: "0.1.0",
      elixir: "~> 1.15",
      # src/nif/clockwork.erl is the module under test; there is no lib/.
      erlc_paths: ["../../src/nif"],
      start_permanent: false,
      deps: []
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end
end
