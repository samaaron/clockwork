// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The decoders behind `clockwork_audio_file.h`.
//!
//! A sample file comes from a user, from the web or from someone else's
//! project: it is the widest door clockwork has to untrusted bytes, and it
//! used to be answered by four single-file C libraries. This crate answers
//! it in Rust, with symphonia doing the decoding and this code doing what a
//! decoder cannot: deciding what a file is, holding its output to a limit,
//! and refusing what a header claims when the file cannot back it up.
//!
//! What comes out is interleaved 32-bit float, however the file stored its
//! samples, handed to an [`Output`] a frame block at a time so the buffer
//! grows with what the file yields and never with what it says.
//!
//! `ffi.rs` is the C ABI, `cpp/clockwork_audio_decode.h`; `container.rs`
//! reads the uncompressed containers by hand.

pub mod container;
pub mod ffi;

use std::io::Cursor;

use symphonia::core::audio::sample::SampleFormat;
use symphonia::core::codecs::audio::well_known::*;
use symphonia::core::codecs::audio::{AudioCodecId, AudioCodecParameters, AudioDecoderOptions};
use symphonia::core::codecs::CodecParameters;
use symphonia::core::errors::Error;
use symphonia::core::formats::probe::Hint;
use symphonia::core::formats::{FormatOptions, FormatReader, TrackType};
use symphonia::core::io::{MediaSource, MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;

/// The container, with the values of `ClockworkAudioFormat`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Format {
    Unknown = 0,
    Wav = 1,
    Aiff = 2,
    Flac = 3,
    W64 = 4,
    Rf64 = 5,
    Ogg = 6,
    Mp3 = 7,
}

/// How samples sat in the file, with the values of `ClockworkAudioEncoding`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Encoding {
    Unknown = 0,
    PcmS16 = 1,
    PcmS24 = 2,
    PcmS32 = 3,
    Float32 = 4,
    Compressed = 5,
    Pcm8 = 6,
    Float64 = 7,
}

/// What a file turned out to be: `ClockworkAudioInfo` without its size field.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Info {
    pub format: u32,
    pub encoding: u32,
    pub channels: u32,
    pub sample_rate: u32,
    pub source_bits: u32,
    /// From a probe, what the header says; from a decode, what came out.
    pub frames: u64,
}

/// Why a file was not decoded, each the `ClockworkStatus` a caller gets.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// Bytes that are no format this build reads: `CLOCKWORK_E_VERSION`.
    NotAudio,
    /// A known container that does not parse: `CLOCKWORK_E_ARG`.
    Malformed,
    /// More float than the limit allows: `CLOCKWORK_E_TOO_BIG`.
    TooBig,
    /// The output could not grow: `CLOCKWORK_E_NOMEM`.
    NoMemory,
}

impl Refusal {
    /// The numeric `ClockworkStatus`.
    pub fn status(self) -> i32 {
        match self {
            Refusal::NotAudio => 2,
            Refusal::Malformed => 1,
            Refusal::TooBig => 7,
            Refusal::NoMemory => 9,
        }
    }
}

/// Where decoded frames go: a growing buffer of the caller's choosing.
pub trait Output {
    /// Append interleaved samples; false when there is no room to grow.
    fn append(&mut self, samples: &[f32]) -> bool;
}

impl Output for Vec<f32> {
    fn append(&mut self, samples: &[f32]) -> bool {
        self.extend_from_slice(samples);
        true
    }
}

/// What the bytes are, from their first bytes alone.
pub fn sniff(bytes: &[u8]) -> Format {
    container::sniff(bytes)
}

/// The headers alone: what the file is and how long it says it is, with an
/// uncompressed file's length bounded by the bytes actually present.
pub fn probe(bytes: &[u8]) -> Result<Info, Refusal> {
    let opened = open(bytes)?;
    Ok(opened.info)
}

/// The whole file, decoded into `out`, with no more than `max_bytes` of float
/// handed over: a header that claims more is refused before a frame is
/// decoded, and a file that yields more is refused the moment it does. The
/// info's frame count is what came out.
pub fn decode(bytes: &[u8], max_bytes: u64, out: &mut dyn Output) -> Result<Info, Refusal> {
    let mut opened = open(bytes)?;
    let bytes_per_frame = u64::from(opened.info.channels) * 4;
    if opened.info.frames.saturating_mul(bytes_per_frame) > max_bytes {
        return Err(Refusal::TooBig);
    }

    let mut decoder = symphonia::default::get_codecs()
        .make_audio_decoder(&opened.params, &AudioDecoderOptions::default()) // gapless on: delay and padding trimmed
        .map_err(|_| Refusal::Malformed)?;

    let mut block: Vec<f32> = Vec::new();
    let mut frames: u64 = 0;
    let mut handed: u64 = 0;
    loop {
        let packet = match opened.reader.next_packet() {
            Ok(Some(p)) => p,
            Ok(None) => break,
            // A file cut short ends here, with what was decoded.
            Err(Error::IoError(_)) => break,
            Err(_) => {
                if frames > 0 {
                    break;
                }
                return Err(Refusal::Malformed);
            }
        };
        if packet.track_id != opened.track_id {
            continue;
        }
        let decoded = match decoder.decode(&packet) {
            Ok(buf) => buf,
            // One bad frame in a stream is skipped, as every player skips it.
            Err(Error::DecodeError(_)) => continue,
            Err(Error::IoError(_)) => break,
            Err(_) => {
                if frames > 0 {
                    break;
                }
                return Err(Refusal::Malformed);
            }
        };
        let n = decoded.frames() as u64;
        if n == 0 {
            continue;
        }
        handed += n * bytes_per_frame;
        if handed > max_bytes {
            return Err(Refusal::TooBig);
        }
        decoded.copy_to_vec_interleaved::<f32>(&mut block);
        if !out.append(&block) {
            return Err(Refusal::NoMemory);
        }
        frames += n;
    }
    opened.info.frames = frames;
    Ok(opened.info)
}

struct Opened<'a> {
    reader: Box<dyn FormatReader + 'a>,
    track_id: u32,
    params: AudioCodecParameters,
    info: Info,
}

fn open(bytes: &[u8]) -> Result<Opened<'_>, Refusal> {
    let format = sniff(bytes);
    if format == Format::Unknown {
        return Err(Refusal::NotAudio);
    }

    let source: Box<dyn MediaSource + '_> = match format {
        Format::Wav | Format::Aiff | Format::Rf64 | Format::W64 => {
            Box::new(container::normalise(bytes, format).ok_or(Refusal::Malformed)?)
        }
        _ => Box::new(Cursor::new(bytes)),
    };
    let mss = MediaSourceStream::new(source, MediaSourceStreamOptions::default());
    let reader = symphonia::default::get_probe()
        .probe(
            &Hint::new(),
            mss,
            FormatOptions::default(),
            MetadataOptions::default(),
        )
        .map_err(|_| Refusal::Malformed)?;

    let track = reader
        .default_track(TrackType::Audio)
        .ok_or(Refusal::Malformed)?;
    let params = match &track.codec_params {
        Some(CodecParameters::Audio(p)) => p.clone(),
        _ => return Err(Refusal::Malformed),
    };
    let channels = params.channels.as_ref().map(|c| c.count()).unwrap_or(0) as u32;
    let sample_rate = params.sample_rate.unwrap_or(0);
    if channels == 0 || sample_rate == 0 {
        return Err(Refusal::Malformed);
    }
    let (encoding, source_bits) = describe(&params);
    let info = Info {
        format: format as u32,
        encoding: encoding as u32,
        channels,
        sample_rate,
        source_bits,
        frames: track.num_frames.unwrap_or(0),
    };
    Ok(Opened {
        track_id: track.id,
        reader,
        params,
        info,
    })
}

/// The encoding and the bits per sample a codec id stands for.
fn describe(p: &AudioCodecParameters) -> (Encoding, u32) {
    let id: AudioCodecId = p.codec;
    let bits = p.bits_per_sample.unwrap_or(0);
    match id {
        CODEC_ID_PCM_U8 | CODEC_ID_PCM_S8 => (Encoding::Pcm8, 8),
        CODEC_ID_PCM_S16LE | CODEC_ID_PCM_S16BE => (Encoding::PcmS16, 16),
        CODEC_ID_PCM_S24LE | CODEC_ID_PCM_S24BE => (Encoding::PcmS24, 24),
        CODEC_ID_PCM_S32LE | CODEC_ID_PCM_S32BE => (Encoding::PcmS32, 32),
        CODEC_ID_PCM_F32LE | CODEC_ID_PCM_F32BE => (Encoding::Float32, 32),
        CODEC_ID_PCM_F64LE | CODEC_ID_PCM_F64BE => (Encoding::Float64, 64),
        CODEC_ID_PCM_ALAW | CODEC_ID_PCM_MULAW => (Encoding::Compressed, 8),
        CODEC_ID_ADPCM_IMA_WAV | CODEC_ID_ADPCM_MS => (Encoding::Compressed, 4),
        CODEC_ID_FLAC => match bits {
            8 => (Encoding::Pcm8, 8),
            16 => (Encoding::PcmS16, 16),
            24 => (Encoding::PcmS24, 24),
            0 => (Encoding::Unknown, 0),
            _ => (Encoding::PcmS32, bits),
        },
        CODEC_ID_MP3 | CODEC_ID_VORBIS => (Encoding::Compressed, 0),
        _ => match p.sample_format {
            Some(SampleFormat::U8) | Some(SampleFormat::S8) => (Encoding::Pcm8, 8),
            Some(SampleFormat::S16) => (Encoding::PcmS16, 16),
            Some(SampleFormat::S24) => (Encoding::PcmS24, 24),
            Some(SampleFormat::S32) => (Encoding::PcmS32, 32),
            Some(SampleFormat::F32) => (Encoding::Float32, 32),
            Some(SampleFormat::F64) => (Encoding::Float64, 64),
            _ => (Encoding::Unknown, bits),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn fixtures() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../test/fixtures/audio")
    }

    fn fixture(name: &str) -> Vec<u8> {
        std::fs::read(fixtures().join(name)).unwrap_or_else(|e| panic!("{name}: {e}"))
    }

    fn all_fixtures() -> Vec<(String, Vec<u8>)> {
        let mut v: Vec<(String, Vec<u8>)> = std::fs::read_dir(fixtures())
            .unwrap()
            .map(|e| e.unwrap().path())
            .filter(|p| {
                !matches!(
                    p.extension().and_then(|e| e.to_str()),
                    Some("f32") | Some("py")
                )
            })
            .map(|p| {
                (
                    p.file_name().unwrap().to_string_lossy().into_owned(),
                    std::fs::read(&p).unwrap(),
                )
            })
            .collect();
        v.sort();
        assert!(v.len() >= 30);
        v
    }

    #[test]
    fn every_fixture_probes_and_decodes_to_the_length_it_says() {
        for (name, bytes) in all_fixtures() {
            let probed = probe(&bytes).unwrap_or_else(|r| panic!("{name}: probe {r:?}"));
            let mut out = Vec::new();
            let decoded = decode(&bytes, u64::MAX, &mut out)
                .unwrap_or_else(|r| panic!("{name}: decode {r:?}"));
            assert_eq!(
                out.len() as u64,
                decoded.frames * u64::from(decoded.channels),
                "{name}"
            );
            // An MP3 with no Xing or LAME header says nothing about its
            // length; the probe says zero and a decode finds out.
            if probed.frames != 0 || !name.starts_with("mp3") {
                assert_eq!(
                    probed.frames, decoded.frames,
                    "{name}: a probe's length is the decode's"
                );
            }
            assert_eq!(probed.format, decoded.format, "{name}");
            assert!(decoded.frames > 0, "{name}");
        }
    }

    #[test]
    fn every_fixture_cut_at_every_97th_byte_is_refused_or_decoded_short() {
        for (name, bytes) in all_fixtures() {
            let whole = decode(&bytes, u64::MAX, &mut Vec::new()).unwrap();
            let mut cut = 1;
            while cut < bytes.len() {
                let mut out = Vec::new();
                match decode(&bytes[..cut], u64::MAX, &mut out) {
                    Ok(info) => {
                        assert_eq!(info.channels, whole.channels, "{name} at {cut}");
                        assert!(info.frames <= whole.frames, "{name} at {cut}");
                        assert_eq!(
                            out.len() as u64,
                            info.frames * u64::from(info.channels),
                            "{name} at {cut}"
                        );
                    }
                    Err(Refusal::Malformed) | Err(Refusal::NotAudio) => {}
                    Err(r) => panic!("{name} at {cut}: {r:?}"),
                }
                cut += 97;
            }
        }
    }

    #[test]
    fn the_limit_holds_a_claim_and_a_yield() {
        let wav = fixture("wav-s16.wav");
        assert_eq!(decode(&wav, 10_000, &mut Vec::new()), Err(Refusal::TooBig));
        assert!(decode(&wav, 4410 * 8, &mut Vec::new()).is_ok());
        let mp3 = fixture("mp3-cbr.mp3");
        assert_eq!(decode(&mp3, 10_000, &mut Vec::new()), Err(Refusal::TooBig));
        // Yield past the claim: the LAME tag's count is trusted up front, then
        // every frame is counted. The no-Xing file claims nothing and is held
        // to the limit as it decodes.
        let noxing = fixture("mp3-noxing.mp3");
        assert_eq!(probe(&noxing).unwrap().frames, 0);
        assert_eq!(
            decode(&noxing, 10_000, &mut Vec::new()),
            Err(Refusal::TooBig)
        );
    }

    #[test]
    fn an_output_that_cannot_grow_is_no_memory() {
        struct Full;
        impl Output for Full {
            fn append(&mut self, _: &[f32]) -> bool {
                false
            }
        }
        assert_eq!(
            decode(&fixture("flac-s16.flac"), u64::MAX, &mut Full),
            Err(Refusal::NoMemory)
        );
    }

    #[test]
    fn what_is_not_audio_is_said_so() {
        assert_eq!(probe(b""), Err(Refusal::NotAudio));
        assert_eq!(probe(b"not audio, a sentence"), Err(Refusal::NotAudio));
        assert_eq!(probe(&[0u8; 64]), Err(Refusal::NotAudio));
        assert_eq!(probe(b"RIFF"), Err(Refusal::NotAudio));
        assert_eq!(probe(b"RIFF\0\0\0\0WAVE"), Err(Refusal::Malformed));
    }

    /// The fuzz seeds and every crasher the fuzzer ever found (corpus/, the
    /// seeds rust/fuzz's audio_decode target starts from), decoded as a
    /// test: deterministic, on every push.
    #[test]
    fn the_fuzz_corpus_replays() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("corpus");
        let mut seen = 0;
        for dir in ["seeds", "regressions"] {
            let Ok(entries) = std::fs::read_dir(root.join(dir)) else {
                continue;
            };
            for e in entries {
                let p = e.unwrap().path();
                if p.file_name()
                    .is_some_and(|n| n.to_string_lossy().starts_with('.'))
                {
                    continue;
                }
                let bytes = std::fs::read(&p).unwrap();
                let _ = probe(&bytes);
                let mut out = Vec::new();
                if let Ok(info) = decode(&bytes, 64 * 1024 * 1024, &mut out) {
                    assert_eq!(
                        out.len() as u64,
                        info.frames * u64::from(info.channels),
                        "{}",
                        p.display()
                    );
                }
                seen += 1;
            }
        }
        assert!(seen > 0, "no corpus at {}", root.display());
    }
}
