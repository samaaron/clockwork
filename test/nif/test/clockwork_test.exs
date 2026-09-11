# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# The NIF as a transport: the lifecycle it promises (asynchronous start and
# stop, reported by message), send_osc as a ring write, and the audience
# registry that every reply, broadcast and debug line fans out to. Brought
# across from supersonic's test/nif, which drove the same surface when the
# NIF was that project's; the two scsynth replies it looked for (/version,
# /g_queryTree) are clockwork's own here, because this build has no scsynth:
# /clockwork/ping, answered on the audio thread, and /clockwork/clock/tempo/get,
# answered on the control thread — one reply from each plane, both landing in
# the same mailbox.
defmodule ClockworkTest do
  use ExUnit.Case

  # CLOCKWORK_HEADLESS=1 boots without an audio device. The engine's own
  # headless driver ticks the audio path; nothing here has to.
  @headless System.get_env("CLOCKWORK_HEADLESS") == "1"

  # One global engine in the NIF, so each test starts from a stopped one.
  # start/stop are asynchronous, so block on the stop here rather than trust
  # that the previous test's teardown has landed.
  setup do
    stop_sync()
    on_exit(fn -> :clockwork.stop() end)
    :ok
  end

  # ── Helpers ────────────────────────────────────────────────────────────────

  defp start_sync(config) do
    assert :ok = :clockwork.start(config)

    receive do
      {:clockwork_started, result} -> result
    after
      5000 -> flunk("start did not report completion")
    end
  end

  defp stop_sync do
    assert :ok = :clockwork.stop()

    receive do
      {:clockwork_stopped, result} -> result
    after
      5000 -> flunk("stop did not report completion")
    end
  end

  defp start_config(overrides \\ %{}) do
    if @headless, do: Map.merge(%{headless: true}, overrides), else: overrides
  end

  # OSC: NUL-terminated strings padded to four bytes, then the type tag.
  defp osc_message(address), do: osc_string(address) <> osc_string(",")

  defp osc_message(address, int_arg) when is_integer(int_arg),
    do: osc_string(address) <> osc_string(",i") <> <<int_arg::signed-big-32>>

  defp osc_message(address, float_arg) when is_float(float_arg),
    do: osc_string(address) <> osc_string(",f") <> <<float_arg::float-big-32>>

  defp osc_string(str) do
    bytes = str <> <<0>>

    pad_len =
      case rem(byte_size(bytes), 4) do
        0 -> 0
        n -> 4 - n
      end

    bytes <> <<0::size(pad_len * 8)>>
  end

  defp wait_for_reply(timeout \\ 2000) do
    receive do
      {:osc_reply, bin} when is_binary(bin) -> {:ok, bin}
    after
      timeout -> :timeout
    end
  end

  # The first reply whose address starts with `prefix`; anything else that
  # arrives first (other replies, debug lines) is let past.
  defp wait_for_reply_matching(prefix, timeout \\ 2000) do
    deadline = System.monotonic_time(:millisecond) + timeout
    do_wait_matching(prefix, deadline)
  end

  defp do_wait_matching(prefix, deadline) do
    remaining = deadline - System.monotonic_time(:millisecond)

    if remaining <= 0 do
      :timeout
    else
      receive do
        {:osc_reply, bin} when is_binary(bin) ->
          if String.starts_with?(bin, prefix),
            do: {:ok, bin},
            else: do_wait_matching(prefix, deadline)

        {:debug, _} ->
          do_wait_matching(prefix, deadline)
      after
        remaining -> :timeout
      end
    end
  end

  # The last argument of a reply whose last argument is an OSC double ('d').
  defp trailing_double(bin) do
    <<_::binary-size(byte_size(bin) - 8), d::float-big-64>> = bin
    d
  end

  # ── Loading ────────────────────────────────────────────────────────────────

  test "NIF is loaded" do
    assert :clockwork.is_nif_loaded() == true
  end

  # ── Lifecycle ──────────────────────────────────────────────────────────────

  test "start and stop" do
    assert :ok = start_sync(start_config())
    assert :ok = stop_sync()
  end

  test "start is asynchronous: returns :ok at once, reports completion by message" do
    assert :ok = :clockwork.start(start_config())
    assert_receive {:clockwork_started, :ok}, 5000
  end

  test "stop is asynchronous: returns :ok at once, reports completion by message" do
    assert :ok = start_sync(start_config())
    assert :ok = :clockwork.stop()
    assert_receive {:clockwork_stopped, :ok}, 5000
  end

  test "a second start reports already_running through the completion message" do
    assert :ok = start_sync(start_config())
    assert :ok = :clockwork.start(start_config())
    assert_receive {:clockwork_started, {:error, :already_running}}, 5000
  end

  test "stop when not running is ok" do
    assert :ok = stop_sync()
  end

  # ── send_osc ───────────────────────────────────────────────────────────────

  test "send_osc when not running returns an error" do
    assert {:error, :not_running} = :clockwork.send_osc(osc_message("/clockwork/ping"))
  end

  test "send_osc with a message returns ok" do
    :ok = start_sync(start_config())
    assert :ok = :clockwork.send_osc(osc_message("/clockwork/ping"))
  end

  test "send_osc with a non-binary is badarg" do
    :ok = start_sync(start_config())
    assert_raise ArgumentError, fn -> :clockwork.send_osc(:not_a_binary) end
  end

  # ── The audience ───────────────────────────────────────────────────────────

  test "set and clear notification pid" do
    assert :ok = :clockwork.set_notification_pid()
    assert :ok = :clockwork.clear_notification_pid()
  end

  test "every registered process receives every reply" do
    :ok = start_sync(start_config())
    test_pid = self()

    # A second process registers and relays the first reply it sees.
    relay =
      spawn(fn ->
        :clockwork.set_notification_pid()
        send(test_pid, :relay_registered)

        receive do
          {:osc_reply, bin} -> send(test_pid, {:relay, bin})
        after
          2000 -> send(test_pid, :relay_timeout)
        end
      end)

    assert_receive :relay_registered, 2000

    :ok = :clockwork.set_notification_pid()
    :ok = :clockwork.send_osc(osc_message("/clockwork/ping"))

    # Both mailboxes get it: the registry fans out, it is not a single pid.
    assert {:ok, _} = wait_for_reply_matching("/clockwork/pong")
    assert_receive {:relay, bin} when is_binary(bin), 2000

    Process.exit(relay, :kill)
  end

  # ── Replies from both planes ───────────────────────────────────────────────

  test "a ping is answered on the audio thread and reaches the registered pid" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()
    :ok = :clockwork.send_osc(osc_message("/clockwork/ping"))

    assert {:ok, reply} = wait_for_reply()
    assert String.starts_with?(reply, "/clockwork/pong")
  end

  test "a clock query travels the control plane and its reply reaches the same pid" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()
    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/tempo/get"))

    assert {:ok, reply} = wait_for_reply_matching("/clockwork/clock/tempo.reply")
    assert is_binary(reply)
  end

  test "a tempo set then get round-trips the value" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()

    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/tempo/set", 142.5))
    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/tempo/get"))

    assert {:ok, reply} = wait_for_reply_matching("/clockwork/clock/tempo.reply")
    assert_in_delta trailing_double(reply), 142.5, 0.5
  end

  test "a visibility query replies to the registered pid" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()
    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/visibility/get"))

    assert {:ok, reply} = wait_for_reply_matching("/clockwork/clock/visibility.reply")
    assert is_binary(reply)
  end

  test "audio-thread and control-thread replies both reach the same pid" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()

    :ok = :clockwork.send_osc(osc_message("/clockwork/ping"))
    assert {:ok, _} = wait_for_reply_matching("/clockwork/pong")

    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/tempo/get"))
    assert {:ok, _} = wait_for_reply_matching("/clockwork/clock/tempo.reply")
  end

  # ── Notify ─────────────────────────────────────────────────────────────────
  #
  # Subscribing pushes a tempo and peers snapshot straight back to the
  # subscriber. The NIF is a real out-of-process peer, so it gets the push
  # exactly as a UDP client does.

  test "subscribing to clock notify pushes an immediate snapshot" do
    :ok = start_sync(start_config())
    :ok = :clockwork.set_notification_pid()
    :ok = :clockwork.send_osc(osc_message("/clockwork/clock/notify/subscribe"))

    assert {:ok, _} = wait_for_reply_matching("/clockwork/clock/notify/tempo")
    assert {:ok, _} = wait_for_reply_matching("/clockwork/clock/notify/peers")
  end

  # ── Config ─────────────────────────────────────────────────────────────────

  test "start with a custom config" do
    config =
      start_config(%{
        sample_rate: 44100,
        num_output_channels: 2,
        num_input_channels: 0,
        max_nodes: 512,
        num_buffers: 256
      })

    assert :ok = start_sync(config)
  end
end
