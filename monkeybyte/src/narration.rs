//! TTS pipeline primitives — pure functions, unit-tested.
//! Ported from ../monkeybyte-electron/src/renderer/voice.js + chat-view.js
//! (§AUDIO-CLOCK, §PREBUFFER, §SPEECH-TEXT fixes of 2026-11-14).

/// Sentence segmentation: split on [.!?…] runs (plus closing quotes/parens)
/// and on blank lines. Returns (complete segments, remainder still buffered).
pub fn drain_segments(text: &str) -> (Vec<String>, String) {
    let mut segments = Vec::new();
    let mut rest = text;
    loop {
        let sentence_end = find_sentence_end(rest);
        let paragraph_end = rest.find("\n\n");
        match (sentence_end, paragraph_end) {
            (Some(s), None) => {
                segments.push(rest[..=s].trim().to_string());
                rest = rest[s + 1..].trim_start();
            }
            (None, Some(p)) => {
                segments.push(rest[..p].trim().to_string());
                rest = rest[p + 2..].trim_start();
            }
            (Some(s), Some(p)) if s < p => {
                segments.push(rest[..=s].trim().to_string());
                rest = rest[s + 1..].trim_start();
            }
            (_, Some(p)) => {
                segments.push(rest[..p].trim().to_string());
                rest = rest[p + 2..].trim_start();
            }
            (None, None) => break,
        }
    }
    (segments, rest.to_string())
}

fn find_sentence_end(s: &str) -> Option<usize> {
    let mut i = 0;
    while i < s.len() {
        let c = s[i..].chars().next()?;
        if matches!(c, '.' | '!' | '?' | '\u{2026}') {
            let mut j = i + c.len_utf8();
            while j < s.len() {
                let c2 = s[j..].chars().next().unwrap();
                if matches!(c2, '.' | '!' | '?' | '\u{2026}') {
                    j += c2.len_utf8();
                } else {
                    break;
                }
            }
            while j < s.len() && matches!(s[j..].chars().next().unwrap(), '"' | '\'' | ')' | ']' | '\u{201D}' | '\u{2019}') {
                j += s[j..].chars().next().unwrap().len_utf8();
            }
            return Some(j - 1);
        }
        i += c.len_utf8();
    }
    None
}

/// §SPEECH-TEXT: markdown read literally by TTS turns one paragraph into
/// 58 seconds of "bullet bullet six bee cee three zero...". Sanitize.
pub fn to_speech_text(markdown: &str) -> String {
    let mut out: Vec<String> = Vec::new();
    for line in markdown.split('\n') {
        let cleaned = line
            .trim_start_matches(|c: char| c.is_whitespace())
            .trim_start_matches(|c: char| matches!(c, '\u{2022}' | '*' | '-' | '\u{2013}' | '\u{2014}'))
            .trim_start_matches(|c: char| c.is_whitespace() || matches!(c, ')' | '.'));
        // Ordered-list markers: "1." / "2)"
        let cleaned = {
            let digits: String = cleaned.chars().take_while(|c| c.is_ascii_digit()).collect();
            let after = &cleaned[digits.len()..];
            if !digits.is_empty() && after.starts_with(['.', ')']) && after[1..].starts_with(' ') {
                after[2..].trim_start().to_string()
            } else if !digits.is_empty() && after.starts_with(['.', ')']) {
                after[1..].trim_start().to_string()
            } else {
                cleaned.to_string()
            }
        };
        out.push(cleaned.to_string());
    }
    let joined = out.join("\n");
    let re_backtick = regex::Regex::new("`([^`]*)`").unwrap();
    let joined = re_backtick.replace_all(&joined, "$1").to_string();
    let re_hash = regex::Regex::new(r"\b[0-9a-f]{7,40}\b").unwrap();
    let joined = re_hash.replace_all(&joined, "a commit").to_string();
    let re_url = regex::Regex::new(r"https?://\S+|www\.\S+").unwrap();
    let joined = re_url.replace_all(&joined, "a link").to_string();
    let re_ws = regex::Regex::new(r"[ \t]+").unwrap();
    re_ws.replace_all(joined.trim(), " ").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drain_splits_sentences_and_keeps_remainder() {
        let (segments, remainder) = drain_segments("First sentence. Second sentence!\n\nFinal incomplete");
        assert_eq!(segments, vec!["First sentence.", "Second sentence!"]);
        assert_eq!(remainder, "Final incomplete");
    }

    #[test]
    fn drain_emits_paragraph_only_text_once() {
        let (first, rest) = drain_segments("A complete paragraph\n\nnext");
        assert_eq!(first, vec!["A complete paragraph"]);
        assert_eq!(rest, "next");
        let (second, _) = drain_segments(&rest);
        assert!(second.is_empty());
    }

    #[test]
    fn drain_handles_runs_and_closing_quotes() {
        // First terminator-run per pass: "Wait..." and "really?" split
        // separately — short segments chain gaplessly, same as the JS.
        let (segments, remainder) = drain_segments("Wait... really? \"Yes.\" No");
        assert_eq!(segments, vec!["Wait...", "really?", "\"Yes.\""]);
        assert_eq!(remainder, "No");
    }

    #[test]
    fn speech_text_strips_markdown_noise() {
        assert_eq!(to_speech_text("- bullet one\n* bullet two"), "bullet one\nbullet two");
        assert_eq!(to_speech_text("1. first\n2) second"), "first\nsecond");
        assert_eq!(to_speech_text("`code` and 6bc30e1"), "code and a commit");
        assert_eq!(to_speech_text("see https://example.com/x now"), "see a link now");
    }
}
