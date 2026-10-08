// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_nif.cpp — BEAM NIF interface for clockwork audio engine
 *
 * Thin shim that exposes ClockworkEngine to Erlang/Elixir via NIF calls.
 * OSC messages come in via send_osc/1, replies go back to the registered
 * Erlang processes via enif_send.  No UDP sockets needed.
 *
 * Shape:
 *   - Global engine instance, pointer guarded by a briefly-held mutex
 *   - PID-based notification, the subscriber registry under its own mutex
 *   - Fresh ErlNifEnv per thread callback (enif_send from non-BEAM threads)
 *
 * Every NIF call is non-blocking. send_osc is a ring-buffer write (microseconds).
 * start/stop only parse + enqueue and return immediately; a single lifecycle
 * worker thread performs the slow init()/shutdown() off all BEAM schedulers and
 * posts the outcome to the calling process as {clockwork_started, Result} /
 * {clockwork_stopped, ok}. So no NIF ever blocks a scheduler — not even a dirty
 * one — and nothing needs the dirty-scheduler flag.
 */

#include "erl_nif.h"
#include "ClockworkEngine.h"
#include "GuestConfigText.h"
#include "IOscTransport.h"
#include "clockwork_nif_front.h"


#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ─── Global state ──────────────────────────────────────────────────────────

// Guards only the g_engine pointer, and is held only for the brief publish /
// unpublish / read of it — never across the slow init()/shutdown() (those run on
// the lifecycle worker with no lock held). So send_osc's acquisition is always
// uncontended-or-microseconds, even while a start/stop is in flight.
static std::mutex g_engine_mutex;
static std::unique_ptr<ClockworkEngine> g_engine;
// The product's front, when it has one (clockwork_nif_front.h): published and
// unpublished with g_engine, under the same lock.
static std::unique_ptr<OscFront> g_front;
// The BEAM is one client of the engine, and this is its token: non-zero, so the
// engine answers it directly (a reply to the sender, where 0 would be a
// broadcast to the notify audience), and a front is offered those replies.
static constexpr uint32_t kBeamToken = 1;

// ─── Subscriber registry ────────────────────────────────────────────────────
//
// The BEAM audience for engine egress. Any number of Erlang processes register
// (set_notification_pid); each receives every reply, broadcast and debug line as
// an {osc_reply, Binary} / {debug, String} message. A process that has died is
// detected lazily — enif_send returns 0 — and only that pid is dropped, never the
// whole audience.
//
// notifyTokens/linkSubscribed mirror the transport's subscriber gates
// so the engine knows whether to bother emitting device- and Link-notify traffic;
// they don't change WHO receives (always the registered pids), only WHETHER the
// engine produces the optional broadcasts. Touched from BEAM scheduler threads
// (register/clear), the lifecycle worker (clear on stop) and the NRT gateway
// thread (deliver), so all access is locked.
struct Subscribers {
    mutable std::mutex   mutex;
    std::vector<ErlNifPid> pids;
    std::set<uint32_t>   notifyTokens;   // gates hasNotifySubscribers()
    bool                 linkSubscribed = false;
    bool                 midiSubscribed = false;
    bool                 gamepadSubscribed = false;
    bool                 oscSubscribed = false;

    void addPid(const ErlNifPid& p) {
        std::lock_guard<std::mutex> lk(mutex);
        for (const auto& e : pids)
            if (enif_compare_pids(&e, &p) == 0) return;  // idempotent
        pids.push_back(p);
    }
    void removePid(const ErlNifPid& p) {
        std::lock_guard<std::mutex> lk(mutex);
        erasePid(p);
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mutex);
        pids.clear();
        notifyTokens.clear();
        linkSubscribed = false;
        midiSubscribed = false;
        gamepadSubscribed = false;
        oscSubscribed = false;
    }

    // Frame {osc_reply, <<bytes>>} and fan out to every registered pid.
    bool deliverOscReply(const uint8_t* data, uint32_t size) {
        return deliverAll([&](ErlNifEnv* env) {
            ERL_NIF_TERM bin;
            uint8_t* buf = enif_make_new_binary(env, size, &bin);
            if (buf) memcpy(buf, data, size);
            return enif_make_tuple2(env, enif_make_atom(env, "osc_reply"), bin);
        });
    }
    // Frame {debug, "..."} and fan out to every registered pid.
    void deliverDebug(const std::string& msg) {
        deliverAll([&](ErlNifEnv* env) {
            return enif_make_tuple2(env, enif_make_atom(env, "debug"),
                enif_make_string(env, msg.c_str(), ERL_NIF_LATIN1));
        });
    }

private:
    // Caller must hold mutex.
    void erasePid(const ErlNifPid& p) {
        pids.erase(std::remove_if(pids.begin(), pids.end(),
            [&](const ErlNifPid& e) { return enif_compare_pids(&e, &p) == 0; }),
            pids.end());
    }

    // Build a fresh message env per pid (enif_send invalidates it), send, and
    // evict pids whose process has gone. Returns true if any pid was registered.
    template <typename MakeMsg>
    bool deliverAll(MakeMsg makeMsg) {
        std::lock_guard<std::mutex> lk(mutex);
        if (pids.empty()) return false;
        std::vector<ErlNifPid> dead;
        for (const auto& p : pids) {
            ErlNifEnv* env = enif_alloc_env();
            if (!env) continue;
            ERL_NIF_TERM msg = makeMsg(env);
            int ok = enif_send(nullptr, const_cast<ErlNifPid*>(&p), env, msg);
            enif_free_env(env);
            if (ok == 0) dead.push_back(p);
        }
        for (const auto& d : dead) erasePid(d);
        return true;
    }
};
static Subscribers g_subs;

// ─── NifTransport — the engine→BEAM egress boundary ─────────────────────────
//
// The BEAM analogue of UdpOscTransport: an IOscTransport injected into the engine
// (via setTransport) so the NIF is a first-class peer, not a shared-memory
// observer. Unlike CallbackTransport it delivers Link broadcasts and the
// networkOnly Link snapshot — a BEAM client can only see what enif_send hands
// it. The NRT gateway is the sole caller of these methods, but
// the subscriber registry it forwards to is locked (BEAM threads mutate it).
class NifTransport : public IOscTransport {
public:
    explicit NifTransport(Subscribers* subs) : mSubs(subs) {}

    // The token is always kBeamToken; the audience is the registered pids.
    // networkOnly is ignored: the NIF is a real peer, so Link snapshots ship.
    bool send(uint32_t, const uint8_t* data, uint32_t size, bool /*networkOnly*/) override {
        return mSubs->deliverOscReply(data, size);
    }
    void broadcastNotify(const uint8_t* data, uint32_t size) override {
        mSubs->deliverOscReply(data, size);
    }
    void broadcastLink(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->linkSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }

    bool hasNotifySubscribers() const override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        return !mSubs->notifyTokens.empty();
    }
    bool subscribeNotify(uint32_t token) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        return mSubs->notifyTokens.insert(token).second;
    }
    // A BEAM process has no port: the caller is subscribed instead.
    bool subscribeNotifyPort(int) override { return false; }
    void unsubscribeNotify(uint32_t token) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->notifyTokens.erase(token);
    }
    void clearNotify() override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->notifyTokens.clear();
    }

    // A BEAM caller is addressable (unlike a pure in-process observer), so Link
    // notify is supported; delivery is gated on linkSubscribed in broadcastLink.
    bool subscribeLink(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->linkSubscribed = true;
        return true;
    }
    void unsubscribeLink(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->linkSubscribed = false;
    }

    // MIDI notify: a BEAM caller is addressable; delivery is gated
    // on midiSubscribed.
    void broadcastMidi(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->midiSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }
    bool subscribeMidi(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->midiSubscribed = true;
        return true;
    }
    void unsubscribeMidi(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->midiSubscribed = false;
    }

    // Gamepad notify: same shape as the MIDI audience.
    void broadcastGamepad(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->gamepadSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }
    bool subscribeGamepad(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->gamepadSubscribed = true;
        return true;
    }
    void unsubscribeGamepad(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->gamepadSubscribed = false;
    }

    // OSC cues heard by the cue server (/external-osc-cue), once a process
    // has subscribed (/clockwork/osc/notify/subscribe).
    void broadcastOsc(const uint8_t* data, uint32_t size) override {
        bool wanted;
        { std::lock_guard<std::mutex> lk(mSubs->mutex); wanted = mSubs->oscSubscribed; }
        if (wanted) mSubs->deliverOscReply(data, size);
    }
    bool subscribeOsc(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->oscSubscribed = true;
        return true;
    }
    void unsubscribeOsc(uint32_t) override {
        std::lock_guard<std::mutex> lk(mSubs->mutex);
        mSubs->oscSubscribed = false;
    }

private:
    Subscribers* mSubs;
};
static NifTransport g_transport(&g_subs);
// What the engine's replies leave by: g_transport, with the front offered each
// one first when there is a front.
static FrontedTransport g_fronted;

// ─── Debug callback (called from worker threads) ────────────────────────────
// Debug rides the engine's onDebug channel (not the transport); fan it out to
// the same BEAM audience as replies.
static void on_debug(const std::string& msg) {
    g_subs.deliverDebug(msg);
}

// ─── Config parsing helper ─────────────────────────────────────────────────

// A map value as the guest's config text spells it: an integer, a float, a
// boolean as 1/0, or a string as given. False for anything else.
static bool value_as_text(ErlNifEnv* env, ERL_NIF_TERM value, std::string& out) {
    int i;
    if (enif_get_int(env, value, &i)) { out = std::to_string(i); return true; }
    ErlNifSInt64 i64;
    if (enif_get_int64(env, value, &i64)) { out = std::to_string((long long)i64); return true; }
    double d;
    if (enif_get_double(env, value, &d)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", d);
        out = buf;
        return true;
    }
    char atom_buf[16];
    if (enif_get_atom(env, value, atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1)) {
        if (strcmp(atom_buf, "true") == 0)  { out = "1"; return true; }
        if (strcmp(atom_buf, "false") == 0) { out = "0"; return true; }
        return false;
    }
    ErlNifBinary bin;
    if (enif_inspect_binary(env, value, &bin)) {
        out.assign(reinterpret_cast<const char*>(bin.data), bin.size);
        return out.find('\n') == std::string::npos;
    }
    return false;
}

// The keys clockwork answers for itself are the device's: rate, channels,
// buffer, headless. EVERY OTHER KEY IS THE GUEST'S and goes into
// Config::guestConfig as a `name=value` line, spelled as the caller spelled
// it (GuestConfigText.h): clockwork does not know the guest's words, and a
// guest that does not know a name refuses the boot with the line, which is
// the error a misspelt option deserves rather than silence.
//
// Returns "" when every key was read, else why the first that could not be was
// refused: a key that is not an atom, a device value that is not an integer, a
// headless that is not true or false, or a guest value that is not an integer,
// a float, true, false or a binary. Nothing is left out of the boot quietly.
static std::string parse_config(ErlNifEnv* env, ERL_NIF_TERM map,
                                ClockworkEngine::Config& cfg) {
    ERL_NIF_TERM key, value;
    ErlNifMapIterator iter;

    if (!enif_map_iterator_create(env, map, &iter, ERL_NIF_MAP_ITERATOR_FIRST))
        return "start/1 takes a map";

    static const struct { const char* name; int ClockworkEngine::Config::* field; } kDeviceKeys[] = {
        { "sample_rate",         &ClockworkEngine::Config::sampleRate },
        { "num_output_channels", &ClockworkEngine::Config::numOutputChannels },
        { "num_input_channels",  &ClockworkEngine::Config::numInputChannels },
        { "buffer_size",         &ClockworkEngine::Config::bufferSize },
    };

    std::string refused;
    for (; refused.empty() && enif_map_iterator_get_pair(env, &iter, &key, &value);
         enif_map_iterator_next(env, &iter)) {
        char key_buf[64];
        if (!enif_get_atom(env, key, key_buf, sizeof(key_buf), ERL_NIF_LATIN1)) {
            char shown[96];
            enif_snprintf(shown, sizeof(shown), "%T", key);
            refused = std::string("start option ") + shown + ": a key is an atom";
            continue;
        }
        bool device = false;
        for (const auto& k : kDeviceKeys) {
            if (strcmp(key_buf, k.name) != 0) continue;
            device = true;
            int int_val;
            if (enif_get_int(env, value, &int_val)) cfg.*k.field = int_val;
            else refused = std::string("start option ") + key_buf + ": takes an integer";
        }
        if (device) continue;
        if (strcmp(key_buf, "headless") == 0) {
            char atom_buf[16];
            if (enif_get_atom(env, value, atom_buf, sizeof(atom_buf), ERL_NIF_LATIN1)
                && (strcmp(atom_buf, "true") == 0 || strcmp(atom_buf, "false") == 0))
                cfg.headless = strcmp(atom_buf, "true") == 0;
            else
                refused = "start option headless: takes true or false";
            continue;
        }
        std::string text;
        if (value_as_text(env, value, text))
            clockwork::guest_config_text::set(cfg.guestConfig, key_buf, text);
        else
            refused = std::string("start option ") + key_buf
                    + ": takes an integer, a float, true, false or a binary";
    }
    enif_map_iterator_destroy(env, &iter);
    return refused;
}

// ─── Lifecycle worker ───────────────────────────────────────────────────────
//
// start()/stop() must never block a BEAM scheduler: opening the audio device and
// joining the engine's threads take real wall-clock time. So those NIFs only
// parse + enqueue a command (microseconds) and return; this single dedicated
// worker thread performs the slow init()/shutdown() off all schedulers. One
// worker ⇒ start/stop are serialised (never two engines fighting over the device)
// without any long-held lock — g_engine_mutex is taken only to publish/unpublish
// the pointer, so a concurrent send_osc never waits on lifecycle work. The
// outcome is posted to the caller as {clockwork_started, ok|{error,Reason}} /
// {clockwork_stopped, ok}.

enum class LifecycleOp { Start, Stop };
struct LifecycleCmd {
    LifecycleOp              op;
    ClockworkEngine::Config cfg;     // Start only
    std::string              refused; // Start only: why the options were refused
    ErlNifPid                pid;
    bool                     notify;  // false = post no outcome message
};

// Lifecycle-worker shared state, heap-allocated once and intentionally never
// freed. erl_nif's on_unload is not reliably called at VM halt (the BEAM does
// not purge the module on a normal `halt`), so a file-static std::mutex /
// std::condition_variable here would reach its destructor with the worker still
// parked in wait(): glibc's pthread_cond_destroy then blocks forever on the live
// waiter, wedging process exit (Linux only; macOS and Windows do not wait). A
// thread member (even a std::jthread, which would block in its join) would
// likewise reach its destructor with the worker parked. Leaking the whole struct sidesteps every such teardown: the
// OS reclaims it at exit, no destructor runs. on_unload, when it does run, still
// wakes + joins the worker through these fields; it just never destroys them.
struct WorkerState {
    std::mutex               mutex;
    std::condition_variable  cv;
    std::deque<LifecycleCmd> queue;
    bool                     exit = false;
    std::thread*             thread = nullptr;   // thread-guard: allow — leaked on purpose, see above
};
static WorkerState* const       g_worker = new WorkerState;

// Post one message to a single pid from this (non-scheduler) worker thread.
template <typename MakeMsg>
static void notify_pid(const ErlNifPid& pid, MakeMsg makeMsg) {
    ErlNifEnv* env = enif_alloc_env();
    if (!env) return;
    ERL_NIF_TERM msg = makeMsg(env);
    enif_send(nullptr, const_cast<ErlNifPid*>(&pid), env, msg);
    enif_free_env(env);
}

static ERL_NIF_TERM make_started(ErlNifEnv* e, ERL_NIF_TERM result) {
    return enif_make_tuple2(e, enif_make_atom(e, "clockwork_started"), result);
}

static void worker_do_start(const LifecycleCmd& cmd) {
    // Options start/1 could not read: nothing is booted, and the caller hears
    // which one.
    if (!cmd.refused.empty()) {
        if (cmd.notify)
            notify_pid(cmd.pid, [&](ErlNifEnv* e) {
                return make_started(e, enif_make_tuple2(e, enif_make_atom(e, "error"),
                    enif_make_string(e, cmd.refused.c_str(), ERL_NIF_LATIN1)));
            });
        return;
    }

    // Serialised on this thread, so reading g_engine for the running-check needs
    // the lock only against a concurrent send_osc, not against another start.
    {
        std::lock_guard<std::mutex> lk(g_engine_mutex);
        if (g_engine && g_engine->isRunning()) {
            if (cmd.notify)
                notify_pid(cmd.pid, [](ErlNifEnv* e) {
                    return make_started(e, enif_make_tuple2(e,
                        enif_make_atom(e, "error"), enif_make_atom(e, "already_running")));
                });
            return;
        }
    }

    std::unique_ptr<ClockworkEngine> engine;
    std::unique_ptr<OscFront> front;
    std::string err;
    try {
        engine = std::make_unique<ClockworkEngine>();
        engine->onDebug = on_debug;
        front = clockwork_nif_make_front(*engine, g_transport);
        g_fronted.attach(&g_transport, front.get());
        engine->setTransport(&g_fronted);
        engine->init(cmd.cfg);  // SLOW — no lock held
        // A boot the guest refused (an option, a heap it could not have)
        // leaves the engine in Error with the reason: not a running engine.
        if (engine->engineState() == EngineState::Error) {
            err = engine->errorReason();
            if (err.empty()) err = "the engine did not start";
        }
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unknown_exception";
    }

    if (!err.empty()) {
        if (engine) engine->shutdown();
        g_fronted.detachFront();
        front.reset();
        engine.reset();
        if (cmd.notify)
            notify_pid(cmd.pid, [&](ErlNifEnv* e) {
                return make_started(e, enif_make_tuple2(e, enif_make_atom(e, "error"),
                    enif_make_string(e, err.c_str(), ERL_NIF_LATIN1)));
            });
        return;
    }

    {   // brief publish
        std::lock_guard<std::mutex> lk(g_engine_mutex);
        g_engine = std::move(engine);
        g_front = std::move(front);
    }

    if (cmd.notify)
        notify_pid(cmd.pid, [](ErlNifEnv* e) { return make_started(e, enif_make_atom(e, "ok")); });
}

// Unpublishes the engine and its front, then takes them down in the order they
// need: the engine stops (no more replies offered to the front), then the front
// (its thread joined while the engine it holds is still there), then the engine
// object. SLOW — no lock held; nothing reaches either once unpublished.
static void take_down_engine() {
    std::unique_ptr<ClockworkEngine> engine;
    std::unique_ptr<OscFront> front;
    {
        std::lock_guard<std::mutex> lk(g_engine_mutex);
        engine = std::move(g_engine);
        front = std::move(g_front);
    }
    if (engine) engine->shutdown();
    g_fronted.detachFront();
    front.reset();
}

static void worker_do_stop(const LifecycleCmd& cmd) {
    take_down_engine();
    g_subs.clear();                // drop the BEAM audience along with the engine

    if (cmd.notify)
        notify_pid(cmd.pid, [](ErlNifEnv* e) {
            return enif_make_tuple2(e, enif_make_atom(e, "clockwork_stopped"),
                                       enif_make_atom(e, "ok"));
        });
}

static void worker_loop() {
    for (;;) {
        LifecycleCmd cmd;
        {
            std::unique_lock<std::mutex> lk(g_worker->mutex);
            g_worker->cv.wait(lk, [] { return g_worker->exit || !g_worker->queue.empty(); });
            if (g_worker->exit) return;  // drop any still-queued ops at VM shutdown
            cmd = std::move(g_worker->queue.front());
            g_worker->queue.pop_front();
        }
        if (cmd.op == LifecycleOp::Start) worker_do_start(cmd);
        else                              worker_do_stop(cmd);
    }
}

static void worker_enqueue(LifecycleCmd cmd) {
    { std::lock_guard<std::mutex> lk(g_worker->mutex); g_worker->queue.push_back(std::move(cmd)); }
    g_worker->cv.notify_one();
}

// ─── NIF functions ─────────────────────────────────────────────────────────

static ERL_NIF_TERM nif_is_loaded(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    return enif_make_atom(env, "true");
}

// Async: parse the config (fast, needs the calling env), then hand the slow boot
// to the lifecycle worker. Returns `ok` immediately; the outcome arrives as
// {clockwork_started, ok | {error, Reason}} to the calling process.
static ERL_NIF_TERM nif_start(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1 || !enif_is_map(env, argv[0])) return enif_make_badarg(env);

    LifecycleCmd cmd;
    cmd.op = LifecycleOp::Start;
    cmd.cfg.udpPort = 0;  // NIF mode: egress is enif_send, not UDP; 0 also means
                          // no SHM segment (-u > 0 is what creates it)
    cmd.refused = parse_config(env, argv[0], cmd.cfg);
    enif_self(env, &cmd.pid);
    cmd.notify = true;
    worker_enqueue(std::move(cmd));

    return enif_make_atom(env, "ok");  // accepted; result delivered as a message
}

// Async: hand the slow teardown (thread joins, device close) to the lifecycle
// worker. Returns `ok` immediately; completion arrives as {clockwork_stopped, ok}.
static ERL_NIF_TERM nif_stop(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    LifecycleCmd cmd;
    cmd.op = LifecycleOp::Stop;
    enif_self(env, &cmd.pid);
    cmd.notify = true;
    worker_enqueue(std::move(cmd));

    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_send_osc(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
    if (argc != 1) return enif_make_badarg(env);

    ErlNifBinary bin;
    if (!enif_inspect_binary(env, argv[0], &bin))
        return enif_make_badarg(env);

    // Brief lock: the worker never holds g_engine_mutex across slow lifecycle
    // work, so this never waits on a boot/teardown — the call stays a fast,
    // scheduler-safe ring write. The lock also keeps stop from freeing the
    // engine mid-send: stop must take the lock to unpublish g_engine, so it
    // blocks until this sendOSC completes.
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    if (!g_engine || !g_engine->isRunning()) {
        return enif_make_tuple2(env,
            enif_make_atom(env, "error"),
            enif_make_atom(env, "not_running"));
    }

    // The front first, when the product has one: it answers what it takes
    // (from its own thread, as a reply), and the rest goes on to the engine.
    // Every reply reaches every registered process (NifTransport).
    const auto size = static_cast<uint32_t>(bin.size);
    if (g_front && g_front->ingress(bin.data, size, kBeamToken))
        return enif_make_atom(env, "ok");
    const auto error = [env](const char* why) {
        return enif_make_tuple2(env, enif_make_atom(env, "error"), enif_make_atom(env, why));
    };
    switch (g_engine->ingest(bin.data, size, kBeamToken)) {
        case CLOCKWORK_OK:        return enif_make_atom(env, "ok");
        case CLOCKWORK_E_FULL:    return error("full");       // no room this moment
        case CLOCKWORK_E_TOO_BIG: return error("too_big");    // never room: bulk goes in the inbox
        case CLOCKWORK_E_ARG:     return enif_make_badarg(env);  // an empty packet
        default:                  return error("not_running");
    }
}

static ERL_NIF_TERM nif_set_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    ErlNifPid self;
    if (!enif_self(env, &self))
        return enif_make_badarg(env);

    g_subs.addPid(self);
    return enif_make_atom(env, "ok");
}

static ERL_NIF_TERM nif_clear_notification_pid(ErlNifEnv* env, int, const ERL_NIF_TERM[]) {
    // Unregister only the calling process; other subscribers keep receiving.
    ErlNifPid self;
    if (!enif_self(env, &self))
        return enif_make_badarg(env);

    g_subs.removePid(self);
    return enif_make_atom(env, "ok");
}

// ─── NIF lifecycle ─────────────────────────────────────────────────────────

static int on_load(ErlNifEnv*, void**, ERL_NIF_TERM) {
    // Spin up the lifecycle worker (idle until the first start). JUCE/audio init
    // is deferred to the worker on the first boot.
    g_worker->exit = false;
    g_worker->thread = new std::thread(worker_loop);   // thread-guard: allow — see WorkerState
    return 0;
}

static void on_unload(ErlNifEnv*, void*) {
    // Stop the lifecycle worker: it finishes any in-flight op, then exits without
    // touching the (shutting-down) BEAM, so no enif_send races VM teardown.
    {
        std::lock_guard<std::mutex> lk(g_worker->mutex);
        g_worker->exit = true;
    }
    g_worker->cv.notify_one();
    if (g_worker->thread) {
        if (g_worker->thread->joinable())
            g_worker->thread->join();
        delete g_worker->thread;
        g_worker->thread = nullptr;
    }

    // Tear down a still-running engine inline (VM is exiting; blocking is fine).
    take_down_engine();
    g_subs.clear();

    // shutdownJuce_GUI() is deliberately not called. on_unload runs on an
    // arbitrary BEAM scheduler thread, where JUCE's MessageManager teardown is
    // unsafe off the message thread (aborts on macOS, hangs on Linux), and it is
    // unneeded at VM shutdown — the process is exiting, so the OS reclaims it.
}

// ─── Function table ────────────────────────────────────────────────────────

// Every function is non-blocking, so none need dirty scheduling: start/stop just
// enqueue to the lifecycle worker and return; send_osc is a ring write.
static ErlNifFunc nif_funcs[] = {
    {"is_nif_loaded",          0, nif_is_loaded,              0},
    {"start",                  1, nif_start,                  0},
    {"stop",                   0, nif_stop,                   0},
    {"send_osc",               1, nif_send_osc,               0},
    {"set_notification_pid",   0, nif_set_notification_pid,   0},
    {"clear_notification_pid", 0, nif_clear_notification_pid, 0},
};

// Hot upgrade intentionally disabled: JUCE/NSApplication teardown is
// not safe from arbitrary BEAM schedulers (see on_unload). The two
// NULLs below are the `reload' and `upgrade' slots. Rationale and
// user-facing docs: clockwork.erl module @doc.
ERL_NIF_INIT(clockwork, nif_funcs, on_load, NULL, NULL, on_unload)
