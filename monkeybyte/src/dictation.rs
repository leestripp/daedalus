//! Dictation — cpal mic capture → 16 kHz mono WAV → ChimpFlow whisper → text.
//! Contract mirrored from monkeybyte-electron/src/main/chimpflow.js:
//! POST {base}/v1/audio/transcriptions, multipart `file` + `model`,
//! Bearer auth when an api key is configured.

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::sync::{Arc, Mutex};
use std::time::Instant;
use crate::log;

pub struct Recording {
    #[allow(dead_code)]
    pub stream: cpal::Stream, // kept alive while recording
    pub samples: Arc<Mutex<Vec<f32>>>,
    pub started: Instant,
    pub channels: u16,
    pub rate: u32,
}

pub fn start() -> anyhow::Result<Recording> {
    let host = cpal::default_host();
    let device = host
        .default_input_device()
        .ok_or_else(|| anyhow::anyhow!("no microphone (no default input device)"))?;
    let supported = device.default_input_config()?;
    let spec = supported.config();
    let channels = spec.channels.max(1);
    let samples = Arc::new(Mutex::new(Vec::<f32>::new()));
    let on_err = |e| log::log(&format!("dictation stream error: {e}"));
    let cb = samples.clone();
    let stream = match supported.sample_format() {
        cpal::SampleFormat::F32 => device.build_input_stream(
            &spec,
            move |data: &[f32], _| {
                if let Ok(mut v) = cb.lock() {
                    v.extend_from_slice(data);
                }
            },
            on_err,
            None,
        )?,
        cpal::SampleFormat::I16 => device.build_input_stream(
            &spec,
            move |data: &[i16], _| {
                if let Ok(mut v) = cb.lock() {
                    v.extend(data.iter().map(|&s| s as f32 / i16::MAX as f32));
                }
            },
            on_err,
            None,
        )?,
        other => anyhow::bail!("unsupported mic sample format {other:?}"),
    };
    stream.play()?;
    log::log(&format!("dictation: recording {}ch {}Hz", channels, spec.sample_rate.0));
    Ok(Recording { stream, samples, started: Instant::now(), channels, rate: spec.sample_rate.0 })
}

/// Interleaved → mono (per-frame average).
pub fn to_mono(samples: &[f32], channels: usize) -> Vec<f32> {
    if channels <= 1 {
        return samples.to_vec();
    }
    let frames = samples.len() / channels;
    (0..frames)
        .map(|i| {
            let frame = &samples[i * channels..(i + 1) * channels];
            frame.iter().sum::<f32>() / channels as f32
        })
        .collect()
}

/// Naive linear resample — fine for speech at small ratios (48k→16k).
pub fn resample_linear(samples: &[f32], from: u32, to: u32) -> Vec<f32> {
    if from == to || samples.is_empty() {
        return samples.to_vec();
    }
    let ratio = from as f64 / to as f64;
    let out_len = (samples.len() as f64 / ratio) as usize;
    (0..out_len)
        .map(|i| {
            let pos = i as f64 * ratio;
            let i0 = pos as usize;
            let i1 = (i0 + 1).min(samples.len() - 1);
            let t = (pos - i0 as f64) as f32;
            samples[i0] * (1.0 - t) + samples[i1] * t
        })
        .collect()
}

pub fn write_wav16(path: &std::path::Path, samples: &[f32], rate: u32) -> anyhow::Result<()> {
    let spec = hound::WavSpec {
        channels: 1,
        sample_rate: rate,
        bits_per_sample: 16,
        sample_format: hound::SampleFormat::Int,
    };
    let mut writer = hound::WavWriter::create(path, spec)?;
    for &s in samples {
        writer.write_sample((s.clamp(-1.0, 1.0) * 32767.0) as i16)?;
    }
    writer.finalize()?;
    Ok(())
}

/// OpenAI-compatible transcription; returns the text (may be empty).
pub fn transcribe(base: &str, api_key: &str, model: &str, wav_path: &std::path::Path) -> anyhow::Result<String> {
    let client = reqwest::blocking::Client::builder()
        .timeout(std::time::Duration::from_secs(90))
        .build()?;
    let form = reqwest::blocking::multipart::Form::new()
        .part(
            "file",
            reqwest::blocking::multipart::Part::file(wav_path)?
                .file_name("dictation.wav")
                .mime_str("audio/wav")?,
        )
        .text("model", model.to_string());
    let mut request = client
        .post(format!("{base}/v1/audio/transcriptions"))
        .multipart(form);
    if !api_key.is_empty() {
        request = request.header("Authorization", format!("Bearer {api_key}"));
    }
    let value: serde_json::Value = request.send()?.json()?;
    Ok(value["text"].as_str().unwrap_or("").to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mono_averages_channels() {
        let stereo = [1.0f32, 0.0, 0.5, 0.5];
        assert_eq!(to_mono(&stereo, 2), vec![0.5, 0.5]);
        assert_eq!(to_mono(&[0.25f32, 0.75], 1), vec![0.25, 0.75]);
    }

    #[test]
    fn resample_halves_length() {
        let s: Vec<f32> = (0..48000).map(|i| (i as f32 * 0.01).sin()).collect();
        let out = resample_linear(&s, 48000, 16000);
        assert!((out.len() as i64 - 16000).abs() <= 1);
        // identity
        assert_eq!(resample_linear(&s, 48000, 48000).len(), s.len());
    }

    #[test]
    fn wav_roundtrip_16bit() {
        let dir = std::env::temp_dir();
        let path = dir.join("mb-dictation-test.wav");
        let samples = vec![0.0f32, 0.5, -0.5, 1.0, -1.0];
        write_wav16(&path, &samples, 16000).unwrap();
        let mut reader = hound::WavReader::open(&path).unwrap();
        assert_eq!(reader.spec().sample_rate, 16000);
        assert_eq!(reader.spec().channels, 1);
        let read: Vec<i16> = reader.samples::<i16>().map(|r| r.unwrap()).collect();
        assert_eq!(read.len(), 5);
        let _ = std::fs::remove_file(&path);
    }
}
