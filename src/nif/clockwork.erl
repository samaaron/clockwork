%% SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
%% Copyright (c) 2025 Sam Aaron
%% @doc clockwork NIF — audio engine interface for BEAM languages.
%%
%% Wraps the clockwork audio engine as a NIF.  OSC messages
%% go in via {@link send_osc/1}, replies come back as Erlang messages
%% to the registered processes (see {@link set_notification_pid/0}).
%%
%% The NIF has no socket: it installs a transport of its own, which hands
%% replies and broadcasts to the registered processes. Device-notify and
%% Link-notify are subscribed to with the usual OSC commands sent through
%% {@link send_osc/1} (e.g. `/clockwork/notify', `/clockwork/clock/notify/subscribe'),
%% and their broadcasts arrive as ordinary `{osc_reply, Binary}' messages.
%%
%% Multiple processes may register; each receives every reply, broadcast
%% and debug line. A reply is not addressed to the process that sent the
%% request: every registered process receives it, whoever asked. A
%% registered process that dies is dropped automatically on the next
%% delivery — the rest keep receiving.
%%
%% Every NIF call is non-blocking and never ties up a BEAM scheduler.
%% {@link send_osc/1} is a ring-buffer write. {@link start/1} and {@link stop/0}
%% are asynchronous: they return `ok' immediately (meaning "accepted") and the
%% slow audio init/teardown runs on a dedicated engine thread. The outcome is
%% delivered to the calling process as a message:
%%   {clockwork_started, ok} | {clockwork_started, {error, Reason}}
%%   {clockwork_stopped, ok}
%%
%% Usage from Elixir:
%%   :clockwork.start(%{headless: true})
%%   receive do
%%     {:clockwork_started, :ok} -> ...
%%     {:clockwork_started, {:error, reason}} -> ...
%%   end
%%   :clockwork.set_notification_pid()
%%   :clockwork.send_osc(osc_binary)
%%   receive do
%%     {:osc_reply, binary} -> ...
%%   end
%%   :clockwork.stop()
%%   receive do
%%     {:clockwork_stopped, :ok} -> ...
%%   end
%%
%% == Hot upgrade not supported ==
%%
%% Hot upgrade of this NIF is not supported. `appup' files involving
%% this module must trigger a full VM restart, not a hot reload. The
%% NIF is loaded with NULL `reload' and `upgrade' callbacks in
%% `ERL_NIF_INIT', so the BEAM rejects hot-upgrade attempts rather
%% than silently accepting them.
%%
%% Calling {@link start/1} and {@link stop/0} repeatedly within a
%% single VM lifetime is supported and tested.
%%
%% Background: the engine spins up a JUCE runtime and, on macOS, an
%% NSApplication / message thread. Their teardown can only run on
%% specific threads; calling `shutdownJuce_GUI()' from an arbitrary
%% BEAM scheduler aborts on macOS.
-module(clockwork).

-export([
    is_nif_loaded/0,
    start/1,
    stop/0,
    send_osc/1,
    set_notification_pid/0,
    clear_notification_pid/0
]).

-on_load(init/0).

%% @private Load the NIF shared library, `clockwork' (the BEAM adds .so or
%% .dll), from the directory named by the CLOCKWORK_NIF_PATH env var; else
%% from the priv directory of the `clockwork' application; else from the
%% directory this module's .beam is in.
init() ->
    Path = case os:getenv("CLOCKWORK_NIF_PATH") of
        false ->
            case code:priv_dir(clockwork) of
                {error, _} ->
                    %% Not running as an OTP app — try relative to beam file
                    Dir = filename:dirname(code:which(?MODULE)),
                    filename:join(Dir, "clockwork");
                PrivDir ->
                    filename:join(PrivDir, "clockwork")
            end;
        NifDir ->
            filename:join(NifDir, "clockwork")
    end,
    erlang:load_nif(Path, 0).

%% @doc Returns `true'. The module loads only with its NIF: if the library
%% cannot be loaded, `-on_load' fails, the module is not loaded, and calling
%% this raises `undef'.
-spec is_nif_loaded() -> true | false.
is_nif_loaded() -> false.

%% @doc Boot the audio engine (asynchronous).
%%
%% Config is a map. The keys clockwork answers for itself are the device's:
%%   sample_rate, num_output_channels, num_input_channels, buffer_size,
%%   headless.
%% EVERY OTHER KEY IS THE GUEST'S, handed to it by name as `name=value'
%% (integers, floats, booleans as 1/0, or a binary). Which names a guest
%% takes is the guest's to say — scsynth lists its own in scsynth_options.h
%% (max_nodes, num_buffers, real_time_memory_size, ...) — and a name it does
%% not know refuses the boot with a message naming it.
%%
%% `headless => true' skips audio device init (for testing/CI).
%%
%% Returns `ok' immediately. The boot runs on a dedicated engine thread (it never
%% blocks a scheduler) and its outcome is sent to the calling process as
%% `{clockwork_started, ok}' or `{clockwork_started, {error, Reason}}'
%% (e.g. `Reason = already_running').
-spec start(Config :: map()) -> ok.
start(_Config) -> erlang:nif_error(nif_not_loaded).

%% @doc Shut down the audio engine (asynchronous).
%%
%% Returns `ok' immediately. Teardown (thread joins, device close) runs on the
%% engine thread and completion is sent to the caller as `{clockwork_stopped, ok}'.
-spec stop() -> ok.
stop() -> erlang:nif_error(nif_not_loaded).

%% @doc Send a raw OSC binary message to the engine.
-spec send_osc(binary()) -> ok | {error, term()}.
send_osc(_OscBinary) -> erlang:nif_error(nif_not_loaded).

%% @doc Register the calling process to receive OSC replies.
%%
%% The registered process will receive messages:
%%   {osc_reply, Binary} — OSC reply data
%%   {debug, String}     — debug output from the engine
%%
%% Any number of processes may register; each receives a copy. Registering
%% the same process twice is a no-op. A registered process that exits is
%% dropped automatically.
-spec set_notification_pid() -> ok.
set_notification_pid() -> erlang:nif_error(nif_not_loaded).

%% @doc Unregister the calling process from OSC reply notifications.
%% Other registered processes are unaffected.
-spec clear_notification_pid() -> ok.
clear_notification_pid() -> erlang:nif_error(nif_not_loaded).

