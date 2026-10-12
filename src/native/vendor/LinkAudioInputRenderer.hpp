/* Copyright 2025, Ableton AG, Berlin. All rights reserved.
 *
 * Receive-only LinkAudio renderer. Adapted from upstream's
 * examples/linkaudio/LinkAudioRenderer.hpp — the receive algorithm and
 * SPSC-queue wiring are upstream's; the send path and bound LinkAudioSink
 * are removed because we manage sinks via our own aux-sink registry
 * (otherwise upstream would announce a phantom "A Sink" channel per
 * subscription).
 *
 * License inherited from upstream Link: GNU GPL v2 or later.
 *
 * Modified for clockwork: the enclosing namespace is `clockwork_link`; the
 * diagnostics (drops, gaps, duplicates, resyncs, warps, drift) are clockwork's;
 * receive() renders only what has arrived; a place the timeline has left
 * behind is given up and found again rather than caught up with; and the
 * stream is read between its samples by libsamplerate's band-limited sinc
 * converter (src/vendor/libsamplerate) rather than upstream's 4-point cubic,
 * from the stream behind its start, so it lands where its stamps put it,
 * sample for sample, from its first.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <ableton/LinkAudio.hpp>
#include <ableton/link_audio/Queue.hpp>
#include <ableton/util/FloatIntConversion.hpp>

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace clockwork_link
{

namespace
{

template <typename T>
T linearInterpolate(T value, T inMin, T inMax, T outMin, T outMax)
{
  return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

} // namespace

// Receive-only LinkAudio buffer renderer.
//   * Constructor takes only `link` (no sink, no sampleRate-by-reference).
//   * Subscribe to a channel via subscribe(channelId).
//   * receive(out, numFrames, sessionState, sampleRate, hostTime, quantum)
//     fills `out` with the channel's audio, aligned to beat time a latency
//     behind, read between its samples by libsamplerate's sinc converter.
//   * Buffers arriving over the network at a different sample rate / tempo
//     are re-pitched by frameIncrement = totalFrames / numFrames.
template <typename Link>
class LinkAudioInputRenderer
{
  // Link Audio's commit() validates numChannels ∈ {1, 2} — see
  // LinkAudio.ipp. Mono sources are mirrored to both internal channels
  // at receive time so receive() always produces a stereo pair and
  // consumers always allocate two consecutive buses.
  static constexpr size_t kChannels = 2;
  // Link's Resizer caps every wire chunk at ~576 bytes (RFC 791 floor;
  // see Encoder.hpp). That's ~125 stereo frames / ~250 mono at int16.
  // 512 has ~4× headroom and survives Link raising the cap toward an
  // MTU-sized payload (~350 stereo frames at 1500-byte MTU).
  // Truncating a delivered buffer breaks Link's beat-time continuity.
  static constexpr size_t kMaxFramesPerBuffer = 512;

  struct Buffer
  {
    std::array<double, kMaxFramesPerBuffer> mSamplesL;
    std::array<double, kMaxFramesPerBuffer> mSamplesR;
    ableton::LinkAudioSource::BufferHandle::Info mInfo;
  };
  using Queue = ableton::link_audio::Queue<Buffer>;

public:
  // How many source frames, from the one it is at, the converter must have
  // to read at `ratio` (output frames per source frame): libsamplerate's
  // medium filter is 22438 coefficients at 491 a source frame, 45.7 frames
  // either side, widened by 1/ratio where it filters for a lower output rate,
  // and it reads on only with more than that in hand (src_sinc.c). A stream
  // is rendered to this short of what has arrived.
  static size_t reach(double ratio)
  {
    return static_cast<size_t>(std::lrint(22438.0 / 491.0 / std::min(1.0, ratio))) + 2;
  }
  // The reach for a stream at `sourceRate` heard at `outputRate`, steered by
  // up to kMaxBend: what receive() renders short of.
  static size_t reach(uint32_t sourceRate, double outputRate)
  {
    return reach(sourceRate > 0 ? outputRate / double(sourceRate) / (1.0 + kMaxBend) : 1.0);
  }

  LinkAudioInputRenderer(Link& link)
    : mLink(link)
  {
    // Depth sized for Live's 2 s lookahead ceiling at Link's wire
    // chunk rate (~780/s for stereo 48 kHz → 1560 needed; 2048 gives
    // ~30 % headroom). Per slot = 2 × kMaxFramesPerBuffer × 8 B = 8 KiB
    // → ~16 MiB per subscription. The 50 ms default uses ~40 slots.
    auto queue = Queue(2048, {});
    mpQueueWriter = std::make_shared<typename Queue::Writer>(std::move(queue.writer()));
    mpQueueReader = std::make_shared<typename Queue::Reader>(std::move(queue.reader()));
    // Everything the converter will need, made now: reading from it never
    // allocates (src/vendor/libsamplerate/CLOCKWORK.md).
    int error = 0;
    mConverter.reset(src_new(SRC_SINC_MEDIUM_QUALITY, static_cast<int>(kChannels), &error));
    if (!mConverter)
      throw std::runtime_error(std::string("libsamplerate: ") + src_strerror(error));
  }

  ~LinkAudioInputRenderer() { mpSource.reset(); }

  LinkAudioInputRenderer(const LinkAudioInputRenderer&) = delete;
  LinkAudioInputRenderer& operator=(const LinkAudioInputRenderer&) = delete;

  bool hasSource() const { return mpSource != nullptr; }

  // Diagnostics — last observed info from the most-recently-delivered
  // buffer. Zero until a buffer arrives. Thread-safe: written on Link
  // thread (single writer), read on any thread.
  uint32_t lastSampleRate() const {
      return mLastSampleRate.load(std::memory_order_relaxed);
  }
  uint32_t lastNumChannels() const {
      return mLastNumChannels.load(std::memory_order_relaxed);
  }
  // Approximate buffered audio in seconds, updated on each receive() pass.
  // 0 if not currently rendering.
  float bufferedSeconds() const {
      return mBufferedSeconds.load(std::memory_order_relaxed);
  }
  // Read rate against the one the two sample rates call for, in ppm (signed),
  // over the last kDriftWindowFrames source frames or more: how hard the two
  // machines' clocks are pulling the resampler. ~0 when they agree, whatever
  // the rates are.
  int32_t lastDriftPpm() const {
      return mLastDriftPpm.load(std::memory_order_relaxed);
  }
  // Times the stream's place on the timeline was lost while rendering and had
  // to be found again: each is a jump in what is heard, or silence until it is.
  uint64_t resyncs() const {
      return mResyncs.load(std::memory_order_relaxed);
  }
  // Renders that left the read position more than kWarpSeconds off where
  // the two sample rates put it: audible as pitch bent, or a skip. The
  // position, not the rate: a render of a few frames can be well off in
  // rate and nowhere near that far off in position.
  uint64_t warps() const {
      return mWarps.load(std::memory_order_relaxed);
  }
  // True if we've received at least one buffer.
  bool everReceived() const {
      return mLastSampleRate.load(std::memory_order_relaxed) > 0;
  }
  // Consumer-side drops (our ring was full at producer push time).
  uint64_t droppedSourceBuffers() const {
      return mDroppedSourceBuffers.load(std::memory_order_relaxed);
  }
  // Upstream/network loss — gaps in BufferHandle::Info::count.
  uint64_t networkGapBuffers() const {
      return mNetworkGapBuffers.load(std::memory_order_relaxed);
  }
  // Raw onSourceBuffer invocation count. Total >> peer publish rate
  // indicates duplicate delivery (multi-interface / retransmits).
  uint64_t totalSourceBufferCalls() const {
      return mTotalSourceBufferCalls.load(std::memory_order_relaxed);
  }
  // Count of invocations whose sequence number equalled the prior
  // call — proves duplicate delivery vs net-new buffers.
  uint64_t duplicateCountCalls() const {
      return mDuplicateCountCalls.load(std::memory_order_relaxed);
  }

  void subscribe(const ableton::ChannelId& channelId)
  {
    mpSource = std::make_unique<ableton::LinkAudioSource>(
      mLink, channelId,
      [this](ableton::LinkAudioSource::BufferHandle bh) { push(bh); });
  }

  // One of the peer's buffers, as the source delivers them (subscribe) or as
  // a test hands them in. Link's thread, or whichever single thread feeds
  // this renderer; never the one calling receive().
  void push(const ableton::LinkAudioSource::BufferHandle& bh)
  {
    // Capture the latest peer-side info for diagnostics regardless of
    // whether we successfully retain a queue slot.
    mLastSampleRate.store(bh.info.sampleRate, std::memory_order_relaxed);
    mLastNumChannels.store(static_cast<uint32_t>(bh.info.numChannels),
                           std::memory_order_relaxed);

    // Gap detection on Link's monotonic per-channel count — surfaces
    // upstream loss before reaching this callback.
    mTotalSourceBufferCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t curCount  = bh.info.count;
    const uint64_t prevCount = mLastSeenCount.load(std::memory_order_relaxed);
    if (prevCount > 0 && curCount > prevCount + 1) {
        mNetworkGapBuffers.fetch_add(curCount - prevCount - 1,
                                      std::memory_order_relaxed);
    } else if (prevCount > 0 && curCount == prevCount) {
        mDuplicateCountCalls.fetch_add(1, std::memory_order_relaxed);
    }
    mLastSeenCount.store(curCount, std::memory_order_relaxed);

    if (!mpQueueWriter->retainSlot()) {
        mDroppedSourceBuffers.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& buffer = *((*mpQueueWriter)[0]);
    buffer.mInfo = bh.info;
    // Source numChannels is 1 or 2 (Link's commit() rejects anything
    // else). Mono → duplicate to both internal channels so receive()
    // always emits stereo.
    const size_t srcChans = bh.info.numChannels;
    const size_t n = std::min(bh.info.numFrames, buffer.mSamplesL.size());
    // Truncation would break Link's beat-time continuity (next
    // buffer's sessionBeatTime advances by the untruncated count).
    // kMaxFramesPerBuffer above is sized to avoid hitting this.
    buffer.mInfo.numFrames = n;
    for (size_t i = 0; i < n; ++i)
    {
      const double l = ableton::util::int16ToFloat<double>(
        bh.samples[i * srcChans]);
      const double r = (srcChans >= 2)
        ? ableton::util::int16ToFloat<double>(bh.samples[i * srcChans + 1])
        : l;
      buffer.mSamplesL[i] = l;
      buffer.mSamplesR[i] = r;
    }
    mpQueueWriter->releaseSlot();
  }

  void unsubscribe()
  {
    if (!mpSource) return;
    mpSource.reset();
    // Drain any queued slots so the next subscription starts clean.
    while (mpQueueReader->retainSlot()) {}
    while (mpQueueReader->numRetainedSlots() > 0) mpQueueReader->releaseSlot();
    moStartReadPos = std::nullopt;
    moFollowOnMicros = std::nullopt;
    mFollow = Stretch{};
    mStretchSeconds = kFirstFollowSeconds;
    mSteering = false;
    mDrift = 0.0;   // the next subscription's peer keeps its own time
    mRate = 1.0;
  }

  // Playback lookahead in wall-clock seconds — consumer plays buffers
  // this far behind wall-clock to absorb network jitter / clock drift.
  // Default 0.05 s matches wired-LAN; Live's UI caps at 2.0 s.
  // Stored as bit-packed atomic<uint64_t> for portable lock-free
  // access (atomic<double> falls back to mutex on some 32-bit ARM).
  void setLatencySeconds(double seconds) {
      if (!(seconds >= 0.0)) seconds = 0.0;
      uint64_t bits;
      std::memcpy(&bits, &seconds, sizeof(bits));
      mLatencySecondsBits.store(bits, std::memory_order_relaxed);
  }
  double latencySeconds() const {
      const uint64_t bits = mLatencySecondsBits.load(std::memory_order_relaxed);
      double seconds;
      std::memcpy(&seconds, &bits, sizeof(seconds));
      return seconds;
  }

  // Audio-thread: fill outL[0..numFrames) and outR[0..numFrames) with
  // audio aligned to (hostTime − latencySeconds) on the Link beat-time
  // grid. Always stereo (mono sources are duplicated). Returns frames
  // filled, fewer than asked when the rest has not arrived yet; un-filled
  // tail is left untouched. Returns 0 when the queue is empty, nothing new
  // has arrived, or buffers don't span the target beat range.
  size_t receive(double* outL,
                 double* outR,
                 size_t numFrames,
                 typename Link::SessionState sessionState,
                 double sampleRate,
                 std::chrono::microseconds hostTime,
                 double quantum)
  {
    // Whether the host went on with the stream: this render asks for the
    // host time the stream's audio so far ends at. A render that gave nothing
    // moved the stream on by nothing, so asks made meanwhile add up, and a
    // host that went on without the stream a device block at a time is told
    // from one that went on with it, however small its steps. Kept on every
    // way out.
    const bool hostFollowedOn = moFollowOnMicros
      && std::fabs(double(hostTime.count()) - *moFollowOnMicros) / 1e6 <= kHostSlackSeconds;
    const auto rendered = [&](size_t frames) {
      if (frames > 0)
        moFollowOnMicros = double(hostTime.count()) + 1e6 * double(frames) / sampleRate;
      return frames;
    };

    // Drain all available slots into our reader half.
    while (mpQueueReader->retainSlot()) {}

    // Convert wall-clock lookahead → beats at the current tempo, so
    // latency stays stable in seconds regardless of BPM.
    const double bpm = sessionState.tempo();
    const double safeBpm = (bpm >= 1.0) ? bpm : 120.0;
    const double kLatencyInBeats =
      latencySeconds() * safeBpm / 60.0;

    const auto targetBeatsAtBufferBegin =
      sessionState.beatAtTime(hostTime, quantum) - kLatencyInBeats;
    // A render's length on from its begin, at the session's tempo. Through
    // whole microseconds it came up to one short, a thirtieth of a frame,
    // made up in the next render: a wobble in the read position every render
    // that put sidebands 38 dB under a 12 kHz tone.
    const auto targetBeatsAtBufferEnd =
      targetBeatsAtBufferBegin + double(numFrames) / sampleRate * safeBpm / 60.0;

    // The place that was kept, against where the timeline now is. They part
    // two ways, and only one is a place lost.
    //
    // Time went by without this being asked — a starved endpoint, a stalled
    // device, a hole in the peer's stream, where renders gave nothing while
    // the host went on — and the audio in between was never heard. It is not
    // caught up with: bending the rate to meet the target in one render plays
    // what was missed many times too fast, a chirp and a burst of clicks. The
    // place is given up (a resync) and found again at the target, as it was
    // found at the start.
    //
    // Or the host went on with the stream, asking for where the last render
    // ended, and the timeline moved under it: Link re-measuring the clock to a
    // peer, which over WiFi with a second peer in the session moves it tens of
    // milliseconds at a time. Nothing was missed and the stream is whole; given
    // up, every measurement was a resync and a hole, a glitch every few seconds
    // through a jam. It is steered to, like the two clocks' disagreement, by
    // at most kMaxBend a render. A move past kMovedSeconds is no measurement
    // but the session itself jumping, and is found again at once.
    if (moStartReadPos && mpQueueReader->numRetainedSlots() > 0)
    {
      const auto& info = (*mpQueueReader)[0]->mInfo;
      const auto begin = info.beginBeats(sessionState, quantum);
      const auto end = info.endBeats(sessionState, quantum);
      if (!begin || !end)
      {
        lose();   // another session's buffers: nothing to hold a place in
      }
      else
      {
        const auto beatsAtRead =
          *begin + (*end - *begin) * (*moStartReadPos / double(info.numFrames));
        const auto secondsOff =
          (targetBeatsAtBufferBegin - beatsAtRead) * 60.0 / safeBpm;
        if (std::fabs(secondsOff) >= (hostFollowedOn ? kMovedSeconds : kLostSeconds)) lose();
      }
    }

    // Drop too-old slots while we're not actively rendering: those that end
    // more than the converter's reach before the target. The reach behind it
    // is kept, so the converter starts from the stream's own audio.
    while (!moStartReadPos && mpQueueReader->numRetainedSlots() > 0)
    {
      const auto& info = (*mpQueueReader)[0]->mInfo;
      const auto begin = info.beginBeats(sessionState, quantum);
      const auto end = info.endBeats(sessionState, quantum);
      const auto reachBeats = (begin && end)
        ? (*end - *begin) * double(reach(info.sampleRate, sampleRate)) / double(info.numFrames)
        : 0.0;
      if (!end || *end + reachBeats < targetBeatsAtBufferBegin)
      {
        mpQueueReader->releaseSlot();
      }
      else break;
    }
    // Nor across a hole: what was kept behind the target is the stream only
    // if it follows on to the slots the target is in.
    while (!moStartReadPos && mpQueueReader->numRetainedSlots() > 1
           && followingOn(sessionState, quantum, safeBpm) == 0)
    {
      const auto next = (*mpQueueReader)[1]->mInfo.beginBeats(sessionState, quantum);
      if (!next || *next > targetBeatsAtBufferBegin) break;
      mpQueueReader->releaseSlot();
    }

    if (mpQueueReader->numRetainedSlots() == 0)
    {
      lose();
      return rendered(0);
    }

    // Wait for buffers to catch up to our target. A target in the crack
    // between two buffers' stamps — the one before dropped as too old, this
    // one beginning a rounding later — is not ahead of what has arrived:
    // this buffer's first frame is the place.
    const auto slackBeats = kHoleSeconds * safeBpm / 60.0;
    if (!moStartReadPos
        && (*mpQueueReader)[0]->mInfo.beginBeats(sessionState, quantum)
             > targetBeatsAtBufferBegin + slackBeats)
    {
      lose();
      return rendered(0);
    }

    // The source frames the converter must have from the one it is at, at
    // the most this stream is read for: a lower output rate than its own
    // widens its filter, and so does steering by kMaxBend.
    const size_t reachFrames = reach((*mpQueueReader)[0]->mInfo.sampleRate, sampleRate);

    if (!moStartReadPos)
    {
      // The slot the target is in, past those kept behind it for the converter.
      double before = 0.0;
      size_t at = 0;
      for (; at + 1 < mpQueueReader->numRetainedSlots(); ++at)
      {
        const auto end = (*mpQueueReader)[at]->mInfo.endBeats(sessionState, quantum);
        if (!end || *end > targetBeatsAtBufferBegin) break;
        before += double((*mpQueueReader)[at]->mInfo.numFrames);
      }
      const auto& info = (*mpQueueReader)[at]->mInfo;
      const auto startBufferBegin = *info.beginBeats(sessionState, quantum);
      const auto startBufferEnd = *info.endBeats(sessionState, quantum);
      double startPos = before + std::max(0.0, linearInterpolate(targetBeatsAtBufferBegin,
                                                                 startBufferBegin, startBufferEnd,
                                                                 0.0, double(info.numFrames)));

      // The converter starts on the stream behind the start, read up to it
      // and thrown away: the first frame heard is the stream there, with the
      // stream before it in the filter, not a fade in from silence. Read up
      // to it at the rate that lands on the start exactly, between two frames
      // as the stamps put it. It needs the reach after the start to have
      // arrived; until it has, nothing is placed. Before the stream's first
      // frame there is nothing, and silence stands in.
      const double from = std::max(0.0, std::floor(startPos) - double(reachFrames));
      const double lead = startPos - from;
      const bool primed = lead >= kPrimeLeastFrames;
      const double steps = std::max(1.0, std::floor(lead));
      const double primeRatio = primed ? steps / lead : 1.0;
      if (startPos + double(std::max(reachFrames, reach(primeRatio)))
            > double(followingOnFrames(sessionState, quantum, safeBpm)))
        return rendered(0);
      src_reset(mConverter.get());
      mFeedPos = static_cast<size_t>(from);
      mFeedLimit = followingOnFrames(sessionState, quantum, safeBpm);
      if (primed && !read(primeRatio, static_cast<size_t>(steps), nullptr, nullptr))
        return rendered(0);   // never: the reach was there to read
      // What the converter holds of the slots behind the start is in it now:
      // they go, and the read position is in the first slot again.
      while (mpQueueReader->numRetainedSlots() > 1
             && startPos >= double((*mpQueueReader)[0]->mInfo.numFrames))
      {
        const auto gone = (*mpQueueReader)[0]->mInfo.numFrames;
        startPos -= double(gone);
        mFeedPos -= gone;
        mpQueueReader->releaseSlot();
      }
      moStartReadPos = startPos;
    }

    const auto startFramePos = *moStartReadPos;

    // Render what has arrived and no further. Asking past the newest buffer is
    // not a place lost but a place not reached yet: the frames that fit are
    // rendered, the rest wait for the next call, and the read position stays.
    // Upstream is only ever asked for what has arrived, from the audio
    // callback a latency behind; clockwork's endpoint runs ahead to keep its
    // port full, so it asks for as much as there is room for.
    //
    // What has arrived is the run of buffers from the current one that
    // follow on from each other. A buffer that does not — the peer's
    // callback stalled, a packet was lost — is a hole, and the stream is
    // rendered up to it and no further: the next call finds the place on
    // the far side a lost one and starts again there. Rendered across it,
    // the hole's edge would have been bent to, and the audio before it
    // thrown away with the render. Following on is to within kHoleSeconds:
    // a publisher's stamps pass through beats and microseconds and abut to
    // within a frame or so, not exactly, and that is one stream, read by its
    // frames as upstream reads it.
    size_t frames = numFrames;
    auto targetBeatsAtEnd = targetBeatsAtBufferEnd;
    const size_t runFrames = followingOnFrames(sessionState, quantum, safeBpm);
    {
      const auto& newest = (*mpQueueReader)[followingOn(sessionState, quantum, safeBpm)]->mInfo;
      const auto newestBegin = *newest.beginBeats(sessionState, quantum);
      const auto newestEnd = *newest.endBeats(sessionState, quantum);
      // The converter's reach short of the end: it reads ahead.
      const auto arrivedUntil =
        newestEnd - double(reachFrames) * (newestEnd - newestBegin) / double(newest.numFrames);
      if (targetBeatsAtEnd > arrivedUntil)
      {
        const auto beatsPerFrame =
          (targetBeatsAtBufferEnd - targetBeatsAtBufferBegin) / double(numFrames);
        const auto fit =
          std::floor((arrivedUntil - targetBeatsAtBufferBegin) / beatsPerFrame);
        if (!(fit >= 1.0)) return rendered(0);   // nothing new to render yet; the place is kept
        frames = std::min(numFrames, static_cast<size_t>(fit));
        targetBeatsAtEnd = targetBeatsAtBufferBegin + beatsPerFrame * double(frames);
      }
    }

    // Where the target begins in the stream, and that its end has arrived.
    const auto targetBegin = positionOf(targetBeatsAtBufferBegin, sessionState, quantum);
    if (!targetBegin || !positionOf(targetBeatsAtEnd, sessionState, quantum))
    {
      lose();
      return rendered(0);
    }
    const auto sourceRate = double((*mpQueueReader)[0]->mInfo.sampleRate);
    const auto framesExpected = sourceRate / sampleRate * double(frames);

    // Read at the rate the two sample clocks keep, changed only between
    // stretches of it.
    //
    // The peer's frames come at its sound card's rate and this engine's go at
    // its own, and the two crystals disagree, by tens of parts per million and
    // slowly: each input's peer its own. Where the target is, worked out
    // afresh each render, carries rounding: Link keeps beats in whole
    // millionths and a publisher stamps its buffers in whole microseconds, a
    // few hundredths of a frame either way. Stepped onto every render, as it
    // was, that rounding wobbled the read position, a tone 38 dB under a
    // 12 kHz sine and 8 Hz from it; and any correction made within one render
    // is a step in phase that spreads round every note.
    //
    // So the target is followed as a clock is (follow()): through where it
    // was over a stretch of kFollowSeconds, render by render, a line is
    // fitted, which averages the rounding away. Its slope is the clocks'
    // disagreement and its end is where the stream really is. The next
    // stretch is read at one rate, the disagreement and whatever closes the
    // distance left over that stretch, and the rate changes only between
    // stretches, by a part per million or so.
    //
    // A target further than kStrayedSeconds is no disagreement of clocks but
    // the timeline moved under the stream (Link re-measuring the clock to a
    // peer) or a step: it is steered out at once, at most kMaxBend of a render
    // at a time, as it always was (upstream bent the whole of it into one
    // render: a yelp), and followed again once it is in place.
    const auto offFrames = *targetBegin - startFramePos;
    const auto steer = follow(offFrames, sourceRate, safeBpm, framesExpected);
    // Only ever within what has arrived; the converter reads its reach ahead.
    double totalFrames = 0.0;
    {
      const auto arrived = double(runFrames) - startFramePos - double(reachFrames);
      const auto most = std::min(framesExpected * (1.0 + kMaxBend), arrived);
      const auto least = framesExpected * (1.0 - kMaxBend);
      if (!(most >= least)) return rendered(0);   // not even a bent render has arrived; the place is kept
      totalFrames = std::clamp(framesExpected * mRate + steer, least, most);
    }
    mFollow.read += totalFrames;
    mFollow.expected += framesExpected;

    const auto frameIncrement = totalFrames / double(frames);
    if (std::fabs(totalFrames - framesExpected)
        > kWarpSeconds * double((*mpQueueReader)[0]->mInfo.sampleRate))
      mWarps.fetch_add(1, std::memory_order_relaxed);
    mDriftFramesRead += totalFrames;
    mDriftFramesExpected += framesExpected;
    if (mDriftFramesExpected >= kDriftWindowFrames)
    {
      const auto bend = mDriftFramesRead / mDriftFramesExpected - 1.0;
      mLastDriftPpm.store(static_cast<int32_t>(std::clamp(bend * 1e6, -2e9, 2e9)),
                          std::memory_order_relaxed);
      mDriftFramesRead = mDriftFramesExpected = 0.0;
    }

    // The converter reads `totalFrames` of the stream, a frameIncrement a
    // frame. At one rate the whole render, as the read position below is
    // kept: libsamplerate would otherwise glide from the last render's rate
    // to this one's over it, and read other frames than these.
    mFeedLimit = runFrames;
    if (!read(1.0 / frameIncrement, frames, outL, outR))
    {
      lose();   // never: the reach was there to read
      return rendered(0);
    }

    // The slots read past go: the converter has what it needs of them.
    auto readPos = startFramePos + totalFrames;
    while (mpQueueReader->numRetainedSlots() > 0
           && readPos >= double((*mpQueueReader)[0]->mInfo.numFrames))
    {
      const auto gone = (*mpQueueReader)[0]->mInfo.numFrames;
      readPos -= double(gone);
      mFeedPos -= gone;
      mpQueueReader->releaseSlot();
    }
    *moStartReadPos = readPos;

    // Buffered-seconds gauge: frames remaining in the current buffer
    // (numFrames - readPos) plus full duration of every queued buffer
    // behind it. moStartReadPos is the read cursor INSIDE the current
    // buffer.
    if (mpQueueReader->numRetainedSlots() > 0) {
      const auto& curInfo = (*mpQueueReader)[0]->mInfo;
      float buffered =
        (float(curInfo.numFrames) - static_cast<float>(*moStartReadPos)) /
        float(curInfo.sampleRate);
      for (size_t i = 1; i < mpQueueReader->numRetainedSlots(); ++i) {
        const auto& info = (*mpQueueReader)[i]->mInfo;
        buffered += float(info.numFrames) / float(info.sampleRate);
      }
      mBufferedSeconds.store(buffered, std::memory_order_relaxed);
    } else {
      mBufferedSeconds.store(0.0f, std::memory_order_relaxed);
    }

    return rendered(frames);
  }

private:
  // A render leaving the read position this far off where the rates put it,
  // in seconds of the source, is a warp: a bend of a twentieth over a
  // chunk, heard. A frame or two is a publisher's stamp rounding, the
  // position taken from a stamp rather than the frames, and not heard.
  static constexpr double kWarpSeconds = 0.0005;
  // A read position this far from the beat asked for, in seconds at the
  // current tempo, is a place the timeline has left behind — a stall, a
  // hole, a device that jumped — not a correction to steer out: given up
  // and found again. Below it the stream is bent toward the target by at
  // most kMaxBend a render, a hundredth, and a two-millisecond correction
  // is gone within a second with nothing heard.
  static constexpr double kLostSeconds = 0.01;
  static constexpr double kMaxBend = 0.01;
  // A render that asks for the host time the stream's audio so far ends at,
  // to within this, follows on: the endpoint dates each render from the reads
  // it fills, to the microsecond, and a device's dates wander a little around
  // that.
  static constexpr double kHostSlackSeconds = 0.002;
  // How far the timeline can move under a stream the host followed on with
  // and still be steered to: a clock re-measurement is tens of milliseconds.
  // At kMaxBend that is ten seconds' bending at most; further is the session
  // itself jumping, and found again at once.
  static constexpr double kMovedSeconds = 0.1;
  // Consecutive buffers whose stamps are this far apart, in seconds at the
  // current tempo, have a hole between them; nearer is rounding, and the
  // frames are trusted over the stamps.
  static constexpr double kHoleSeconds = 0.001;
  // Drift is the read rate over at least this many source frames, ~0.7 s at
  // 48 kHz: long enough to be the clocks' disagreement rather than the
  // steering of one callback.
  static constexpr double kDriftWindowFrames = 32768.0;
  // A target this near the read position is reached: more than the rounding
  // in where the target is worked out to be (Link's millionths of a beat, a
  // publisher's whole microseconds), and nothing heard.
  static constexpr double kSteadySeconds = 0.000002;
  static constexpr double kSteadyBeats = 0.000002;
  // The stretch the target is followed over, once it is followed; and the
  // first, from which each is twice the last until it is that long. The
  // clocks' disagreement is unknown at first, and over half a second at an
  // unknown tens of parts per million the stream would fall a frame behind:
  // it is learned roughly at once, then closely.
  static constexpr double kFollowSeconds = 0.5;
  static constexpr double kFirstFollowSeconds = kFollowSeconds / 16.0;
  // The distance a line leaves between the target and the stream is closed
  // over this many stretches, not the next alone: where a line ends carries
  // the most rounding, and closed in one stretch it moved the rate by parts
  // per million, a kink in the read position at every stretch.
  static constexpr double kCloseStretches = 4.0;
  static constexpr double kMaxDrift = 0.001;
  // A target further than this from the read position has strayed: over a
  // stretch the clocks part by tens of microseconds at most.
  static constexpr double kStrayedSeconds = 0.0005;
  // A start nearer the stream's first frame than this is at it: the
  // converter cannot be read up to it at a ratio libsamplerate takes (at
  // most 256 output frames a source frame).
  static constexpr double kPrimeLeastFrames = 1.0 / 128.0;
  // Frames the converter is offered, and asked for, at a time.
  static constexpr size_t kChunkFrames = 256;

  // The last of the run of slots from the first that follow on from each
  // other: the stream up to its first hole, as far as it has arrived.
  size_t followingOn(const typename Link::SessionState& sessionState, double quantum, double bpm) const
  {
    size_t last = 0;
    const auto first = (*mpQueueReader)[0]->mInfo.endBeats(sessionState, quantum);
    if (!first) return 0;
    auto lastEnd = *first;
    for (size_t i = 1; i < mpQueueReader->numRetainedSlots(); ++i)
    {
      const auto& info = (*mpQueueReader)[i]->mInfo;
      const auto begin = info.beginBeats(sessionState, quantum);
      const auto end = info.endBeats(sessionState, quantum);
      if (!begin || !end) break;   // another session's: not this stream
      if (std::fabs(*begin - lastEnd) * 60.0 / bpm > kHoleSeconds) break;
      last = i;
      lastEnd = *end;
    }
    return last;
  }

  // The frames in that run.
  size_t followingOnFrames(const typename Link::SessionState& sessionState, double quantum, double bpm) const
  {
    const size_t last = followingOn(sessionState, quantum, bpm);
    size_t frames = 0;
    for (size_t i = 0; i <= last; ++i) frames += (*mpQueueReader)[i]->mInfo.numFrames;
    return frames;
  }

  // `frames` of the stream from the converter at `ratio`, output frames per
  // source frame, into outL and outR (or thrown away, with neither). It is
  // given what has arrived, up to mFeedLimit, and takes what it needs;
  // never told the stream has ended, which it would pad with silence. False
  // if it needed more than has arrived, which receive never lets it.
  bool read(double ratio, size_t frames, double* outL, double* outR)
  {
    src_set_ratio(mConverter.get(), ratio);   // the whole read at this ratio, not a glide to it
    SRC_DATA data{};
    data.src_ratio = ratio;
    for (size_t done = 0; done < frames;)
    {
      data.data_in = mIn.data();
      data.input_frames = static_cast<long>(stage());
      data.data_out = mOut.data();
      data.output_frames = static_cast<long>(std::min(kChunkFrames, frames - done));
      if (src_process(mConverter.get(), &data) != 0) return false;
      mFeedPos += static_cast<size_t>(data.input_frames_used);
      const auto got = static_cast<size_t>(data.output_frames_gen);
      if (got == 0 && data.input_frames_used == 0) return false;
      if (outL)
      {
        for (size_t i = 0; i < got; ++i)
        {
          outL[done + i] = mOut[kChannels * i];
          outR[done + i] = mOut[kChannels * i + 1];
        }
      }
      done += got;
    }
    return true;
  }

  // The stream from the feed position into mIn, as much as fits and no
  // further than mFeedLimit, interleaved as libsamplerate takes it. Frames
  // in it the converter does not take are staged again next time.
  size_t stage()
  {
    const size_t n = mFeedLimit > mFeedPos ? std::min(kChunkFrames, mFeedLimit - mFeedPos) : 0;
    size_t slot = 0;
    size_t idx = mFeedPos;
    while (n > 0 && idx >= (*mpQueueReader)[slot]->mInfo.numFrames)
    {
      idx -= (*mpQueueReader)[slot]->mInfo.numFrames;
      ++slot;
    }
    for (size_t i = 0; i < n; ++i)
    {
      const auto& buffer = *((*mpQueueReader)[slot]);
      mIn[kChannels * i]     = static_cast<float>(buffer.mSamplesL[idx]);
      mIn[kChannels * i + 1] = static_cast<float>(buffer.mSamplesR[idx]);
      if (++idx == buffer.mInfo.numFrames)
      {
        idx = 0;
        ++slot;
      }
    }
    return n;
  }

  // Where `beats` falls in the stream, in source frames from the first
  // retained slot's first: the frames of the slots before it and its place
  // between the stamps of the one it is in. By the frames, not the stamps: a
  // beat in the rounding crack between two slots' stamps is the second's
  // first frame. Before the first slot is before it, as its stamps say.
  // Nothing past the retained slots, or in another session's.
  std::optional<double> positionOf(double beats,
                                   const typename Link::SessionState& sessionState,
                                   double quantum) const
  {
    double before = 0.0;
    for (size_t i = 0; i < mpQueueReader->numRetainedSlots(); ++i)
    {
      const auto& info = (*mpQueueReader)[i]->mInfo;
      const auto begin = info.beginBeats(sessionState, quantum);
      const auto end = info.endBeats(sessionState, quantum);
      if (!begin || !end) return std::nullopt;
      if (beats < *end)
      {
        const auto at = linearInterpolate(beats, *begin, *end, 0.0, double(info.numFrames));
        return before + (i == 0 ? std::min(at, double(info.numFrames))
                                : std::clamp(at, 0.0, double(info.numFrames)));
      }
      before += double(info.numFrames);
    }
    return std::nullopt;
  }

  // The follower (see receive): takes this render's measure of the target,
  // `offFrames` past the read position, and says what to steer by beyond
  // the read rate mRate — nothing while it follows; the distance, at most
  // kMaxBend of the render, while a strayed target is steered to. At the end
  // of a stretch it fits the line through the stretch's measures and sets
  // the rate for the next.
  double follow(double offFrames, double sourceRate, double bpm, double framesExpected)
  {
    if (std::fabs(offFrames) > kStrayedSeconds * sourceRate) mSteering = true;
    if (mSteering)
    {
      mFollow = Stretch{};   // nothing is learned while it is steered to
      mStretchSeconds = kFirstFollowSeconds;
      mRate = 1.0 + mDrift;
      const auto reached = std::max(kSteadySeconds, kSteadyBeats * 60.0 / bpm) * sourceRate;
      if (std::fabs(offFrames) > reached)
        return std::clamp(offFrames, -kMaxBend * framesExpected, kMaxBend * framesExpected);
      mSteering = false;
    }
    // The target as a point on the stretch: the frames the rates have called
    // for since it began, and how far the target has come beyond them.
    const auto u = mFollow.expected;
    const auto v = mFollow.read + offFrames - u;
    if (u >= mStretchSeconds * sourceRate && mFollow.n >= 2)
    {
      mFollow.add(u, v);
      const auto n = double(mFollow.n);
      const auto slope = (n * mFollow.suv - mFollow.su * mFollow.sv)
                       / (n * mFollow.suu - mFollow.su * mFollow.su);
      const auto at = (mFollow.sv - slope * mFollow.su) / n;
      // Where the line has the target now, past where the stream is read to.
      const auto left = at + slope * u - (mFollow.read - u);
      mDrift = std::clamp(slope, -kMaxDrift, kMaxDrift);
      mStretchSeconds = std::min(2.0 * mStretchSeconds, kFollowSeconds);
      mRate = 1.0 + mDrift
            + std::clamp(left / (kCloseStretches * mStretchSeconds * sourceRate), -kMaxDrift, kMaxDrift);
      mFollow = Stretch{};
      mFollow.add(0.0, offFrames);
      return 0.0;
    }
    mFollow.add(u, v);
    return 0.0;
  }

  // The read position is given up: the next receive finds it from the queue.
  // A resync if it was rendering — then the place was lost, not waited for.
  // The clocks' disagreement as followed stays: they are the same clocks.
  void lose()
  {
    if (moStartReadPos) mResyncs.fetch_add(1, std::memory_order_relaxed);
    moStartReadPos = std::nullopt;
    mFollow = Stretch{};
    mStretchSeconds = kFirstFollowSeconds;
    mSteering = false;
    mRate = 1.0 + mDrift;
    mDriftFramesRead = mDriftFramesExpected = 0.0;
  }

  struct ConverterDelete
  {
    void operator()(SRC_STATE* converter) const { src_delete(converter); }
  };

  Link& mLink;
  std::unique_ptr<ableton::LinkAudioSource> mpSource;
  std::shared_ptr<typename Queue::Writer> mpQueueWriter;
  std::shared_ptr<typename Queue::Reader> mpQueueReader;
  // The stream read between its samples; what it has taken, in source frames
  // from the first retained slot's first, and how far it may be fed; and the
  // two buffers it reads from and writes to. receive() only.
  std::unique_ptr<SRC_STATE, ConverterDelete> mConverter;
  size_t mFeedPos = 0;
  size_t mFeedLimit = 0;
  std::array<float, kChannels * kChunkFrames> mIn{};
  std::array<float, kChannels * kChunkFrames> mOut{};
  std::optional<double> moStartReadPos;
  // The host time, in microseconds, the stream's audio so far ends at: the
  // next render follows on by asking for it. receive() only.
  std::optional<double> moFollowOnMicros;
  // Source frames read, and the frames the rates called for, since drift was
  // last published. receive() only.
  double mDriftFramesRead = 0.0;
  double mDriftFramesExpected = 0.0;
  // The follower (follow()): the two sample clocks' disagreement as last
  // fitted, the fraction faster than the rates say the peer's comes; the
  // rate the stream is read at, against the rates; whether a strayed target
  // is being steered to; and the stretch being followed: the frames read and
  // called for since it began, the sums its line is fitted from, and how
  // long it is to be.
  // receive() only.
  struct Stretch
  {
    double read = 0.0;
    double expected = 0.0;
    size_t n = 0;
    double su = 0.0, sv = 0.0, suu = 0.0, suv = 0.0;
    void add(double u, double v)
    {
      ++n;
      su += u;
      sv += v;
      suu += u * u;
      suv += u * v;
    }
  };
  double mDrift = 0.0;
  double mRate = 1.0;
  bool mSteering = false;
  Stretch mFollow;
  double mStretchSeconds = kFirstFollowSeconds;

  // Diagnostics — updated by the Link-thread callback / audio thread,
  // read by app-thread status queries. Atomic for safe cross-thread reads.
  std::atomic<uint32_t> mLastSampleRate{0};
  std::atomic<uint32_t> mLastNumChannels{0};
  std::atomic<float>    mBufferedSeconds{0.0f};
  std::atomic<int32_t>  mLastDriftPpm{0};
  std::atomic<uint64_t> mResyncs{0};
  std::atomic<uint64_t> mWarps{0};
  std::atomic<uint64_t> mDroppedSourceBuffers{0};
  std::atomic<uint64_t> mNetworkGapBuffers{0};
  std::atomic<uint64_t> mLastSeenCount{0};
  std::atomic<uint64_t> mTotalSourceBufferCalls{0};
  std::atomic<uint64_t> mDuplicateCountCalls{0};
  // Wall-clock playback lookahead. 50 ms default sized for wired-LAN
  // jitter; configurable up to Live's 2 s ceiling via setLatencySeconds.
  // Stored as bit-packed uint64_t for portable lock-free atomic access
  // (atomic<double> falls back to mutex on some 32-bit ARM targets).
  // Initial value below = bit pattern for 0.05.
  std::atomic<uint64_t> mLatencySecondsBits{[](){
      double d = 0.05;
      uint64_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      return bits;
  }()};
};

} // namespace clockwork_link
