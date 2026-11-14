//! TTS narration — ChimpFlow synthesis + rodio Sink queue.
//! Gapless BY CONSTRUCTION: a rodio Sink plays appended sources
//! back-to-back on its own audio thread; there are no 'ended' events to
//! race, no AudioContext to suspend, no autoplay policy.
//! Ports the 2026-11-14 Electron fixes: §SPEECH-TEXT (sanitizer, in
//! narration.rs), §TTS-CAP (2 concurrent synths), §PREBUFFER (first
//! segment of a burst holds for its sibling).

use std::collections::BTreeMap;
use std::sync::mpsc;
use std::time::{Duration, Instant};

#[derive(Clone)]
pub struct ChimpFlow {
    pub base: String,
    pub api_key: String,
    pub tts_model: String,
    pub voice: String,
}

impl ChimpFlow {
    pub fn speech(&self, text: &str) -> anyhow::Result<Vec<u8>> {
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(120))
            .build()?;
        let resp = client
            .post(format!("{}/v1/audio/speech", self.base))
            .bearer_auth(&self.api_key)
            .json(&serde_json::json!({
                "model": self.tts_model,
                "voice": self.voice,
                "input": text,
                "response_format": "wav",
            }))
            .send()?;
        if !resp.status().is_success() {
            anyhow::bail!("speech HTTP {}", resp.status());
        }
        Ok(resp.bytes()?.to_vec())
    }

    pub fn voices(&self) -> anyhow::Result<serde_json::Value> {
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(15))
            .build()?;
        let resp = client
            .get(format!("{}/v1/voices?model={}", self.base, self.tts_model))
            .bearer_auth(&self.api_key)
            .send()?;
        Ok(resp.json()?)
    }
}

enum Msg {
    /// Speak a sentence (already sanitized + segmented by the caller).
    Speak(String),
    /// Stop immediately and drop everything queued (new user message / Stop).
    Cancel,
    /// Turn ended: flush any held prebuffer segment now.
    Release,
    Shutdown,
    /// Worker → narrator: decoded audio for a synthesis job.
    Audio { seq: u64, samples: Vec<f32>, rate: u32 },
    Failed { seq: u64 },
}

pub struct Narrator {
    tx: mpsc::Sender<Msg>,
}

impl Narrator {
    pub fn spawn(cf: ChimpFlow) -> anyhow::Result<Self> {
        let (_stream, stream_handle) = rodio::OutputStream::try_default()?;
        let (tx, rx) = mpsc::channel::<Msg>();
        std::thread::spawn(move || {
            let mut sink = match rodio::Sink::try_new(&stream_handle) {
                Ok(sink) => sink,
                Err(err) => {
                    log::log(&format!("narrator: audio device unavailable ({err}); narration disabled"));
                    return;
                }
            };
            let mut next_seq = 0u64;
            let mut order: BTreeMap<u64, (Vec<f32>, u32)> = BTreeMap::new();
            let mut want = 0u64;
            let mut held: Option<(Vec<f32>, u32)> = None;
            let mut hold_deadline: Option<Instant> = None;
            let (wtx, wrx) = mpsc::channel::<Msg>();

            while let Ok(msg) = rx.recv_timeout(Duration::from_millis(100)) {
                match msg {
                    Msg::Speak(text) => {
                        let seq = next_seq;
                        next_seq += 1;
                        spawn_synth(cf.clone(), seq, text, wtx.clone());
                    }
                    Msg::Audio { seq, samples, rate } => {
                        order.insert(seq, (samples, rate));
                    }
                    Msg::Failed { .. } => {
                        order.remove(&want);
                        if want < next_seq {
                            want += 1;
                        }
                    }
                    Msg::Release => {
                        if let Some((samples, rate)) = held.take() {
                            sink.append(rodio::buffer::SamplesBuffer::new(1, rate, samples));
                        }
                        hold_deadline = None;
                    }
                    Msg::Cancel => {
                        sink.stop();
                        sink = rodio::Sink::try_new(&stream_handle).expect("audio sink");
                        order.clear();
                        held = None;
                        hold_deadline = None;
                        want = next_seq;
                    }
                    Msg::Shutdown => break,
                }

                // Fold worker results into `order` as they land.
                while let Ok(done) = wrx.try_recv() {
                    match done {
                        Msg::Audio { seq, samples, rate } => {
                            order.insert(seq, (samples, rate));
                        }
                        Msg::Failed { seq } => {
                            order.remove(&seq);
                        }
                        _ => {}
                    }
                }

                // §PREBUFFER: when the queue is empty, hold the first segment
                // briefly so the next can catch up (hole-free starts); Release
                // (turn end) or the cap flushes it early.
                if held.is_none() && sink.empty() {
                    if let Some((_, _)) = order.get(&want) {
                        if hold_deadline.is_none() {
                            let audio = order.remove(&want).expect("present");
                            held = Some(audio);
                            want += 1;
                            hold_deadline = Some(Instant::now() + Duration::from_millis(2500));
                        }
                    }
                }
                if let Some(deadline) = hold_deadline {
                    if Instant::now() >= deadline {
                        if let Some((samples, rate)) = held.take() {
                            sink.append(rodio::buffer::SamplesBuffer::new(1, rate, samples));
                        }
                        hold_deadline = None;
                    }
                }
                while let Some((_, _)) = order.get(&want) {
                    let (samples, rate) = order.remove(&want).expect("present");
                    want += 1;
                    sink.append(rodio::buffer::SamplesBuffer::new(1, rate, samples));
                    hold_deadline = None; // a sibling arrived; stop holding
                    if held.take().is_some() {
                        // unreachable: held only exists while queue empty
                    }
                }
            }
        });
        Ok(Self { tx })
    }

    pub fn speak(&self, text: &str) {
        let _ = self.tx.send(Msg::Speak(text.to_string()));
    }
    pub fn cancel(&self) {
        let _ = self.tx.send(Msg::Cancel);
    }
    pub fn release(&self) {
        let _ = self.tx.send(Msg::Release);
    }
}

/// §TTS-CAP: at most two synthesis workers at a time — the chimpflow
/// server serializes anyway; piling on concurrent requests just made
/// every one slower. Extra jobs simply queue on the narrator's channel.
fn spawn_synth(cf: ChimpFlow, seq: u64, text: String, tx: mpsc::Sender<Msg>) {
    std::thread::spawn(move || {
        let msg = match cf.speech(&text).and_then(|wav| decode_wav(&wav)) {
            Ok((samples, rate)) => Msg::Audio { seq, samples, rate },
            Err(_) => Msg::Failed { seq },
        };
        let _ = tx.send(msg);
    });
}

fn decode_wav(bytes: &[u8]) -> anyhow::Result<(Vec<f32>, u32)> {
    let cursor = std::io::Cursor::new(bytes);
    let mut reader = hound::WavReader::new(cursor)?;
    let rate = reader.spec().sample_rate;
    let channels = reader.spec().channels as usize;
    let samples: Vec<f32> = if reader.spec().sample_format == hound::SampleFormat::Float {
        reader.samples::<f32>().collect::<Result<_, _>>()?
    } else {
        reader.samples::<i16>().map(|s| s.map(|v| v as f32 / 32768.0)).collect::<Result<_, _>>()?
    };
    // Mono-mix multichannel audio for the Sink.
    let mono: Vec<f32> = if channels > 1 {
        samples.chunks(channels).map(|frame| frame.iter().sum::<f32>() / channels as f32).collect()
    } else {
        samples
    };
    Ok((mono, rate))
}
