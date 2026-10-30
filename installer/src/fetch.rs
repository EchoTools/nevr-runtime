//! Step 1 and the download half of step 8: every read stops after `max` bytes whatever the server announces, redirects
//! are not followed, and a range request is accepted only as a `206` for exactly the range asked.
//! `file://` bases read a local directory, for tests and for an offline mirror: the signature and hashes still decide.

use crate::util::{redact_text, redact_url};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};

pub const DEFAULT_BASE: &str = "https://evr.echo.taxi/nevr-runtime";

pub enum Failure {
    /// The transport broke (connection, timeout, a short read): a partial file may be kept for a resume.
    Interrupted(String),
    /// The server's answer is refused (status, too many bytes, range not honoured): nothing is kept.
    Refused(String),
}

/// `http://` is fetched only for the local machine itself: the host, after the scheme and before the port or path, is
/// exactly `127.0.0.1` or `localhost`. A name that merely starts like one (`127.0.0.1.invalid`), a userinfo
/// that hides the real host (`127.0.0.1@example.com`) or a stray character is not loopback.
fn is_loopback_http(url: &str) -> bool {
    let Some(rest) = url.strip_prefix("http://") else { return false };
    let authority = rest.split(['/', '?', '#']).next().unwrap_or("");
    let host = match authority.rsplit_once(':') {
        Some((h, port)) if !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit()) => h,
        Some((_, "")) => return false,
        _ => authority,
    };
    matches!(host, "127.0.0.1" | "localhost")
}

// Every message below names the URL only through `redact_url` / `redact_text`: a base can carry credentials (`--base`),
// and a diagnostic is logged (logging standard, Rule 4). The request itself keeps the URL it was given.
fn open(url: &str, start: u64) -> Result<Box<dyn Read>, Failure> {
    let shown = redact_url(url);
    if let Some(path) = url.strip_prefix("file://") {
        let mut f = File::open(path).map_err(|e| Failure::Refused(format!("{shown}: {e}")))?;
        f.seek(SeekFrom::Start(start)).map_err(|e| Failure::Refused(format!("{shown}: {e}")))?;
        return Ok(Box::new(f));
    }
    if !(url.starts_with("https://") || is_loopback_http(url)) {
        return Err(Failure::Refused(format!("{shown}: only https is fetched (http only for the local machine)")));
    }
    let agent = ureq::AgentBuilder::new()
        .redirects(0)
        .timeout_connect(std::time::Duration::from_secs(20))
        .timeout_read(std::time::Duration::from_secs(60))
        .build();
    let mut req = agent.get(url);
    if start > 0 {
        req = req.set("Range", &format!("bytes={start}-"));
    }
    let resp = match req.call() {
        Ok(r) => r,
        Err(ureq::Error::Status(code, _)) => return Err(Failure::Refused(format!("{shown}: HTTP status {code}"))),
        Err(e) => return Err(Failure::Interrupted(format!("could not fetch {shown}: {}", redact_text(&e.to_string(), url)))),
    };
    let ok = if start == 0 {
        resp.status() == 200
    } else {
        resp.status() == 206 && resp.header("Content-Range").is_some_and(|r| r.starts_with(&format!("bytes {start}-")))
    };
    if !ok {
        return Err(Failure::Refused(format!("{shown}: HTTP status {} is not the answer to a request starting at {start}", resp.status())));
    }
    Ok(Box::new(resp.into_reader()))
}

/// Reads `url` from byte `start`, writes at most `max_total - start` bytes to `sink` and returns how many. More is
/// `Refused`, whether or not a length was announced.
pub fn fetch_capped(url: &str, start: u64, max_total: u64, sink: &mut dyn Write) -> Result<u64, Failure> {
    let room = max_total.saturating_sub(start);
    let mut reader = open(url, start)?.take(room + 1);
    let (mut size, mut buf) = (0u64, vec![0u8; 65536]);
    loop {
        let n = reader.read(&mut buf).map_err(|e| Failure::Interrupted(format!("{}: {}", redact_url(url), redact_text(&e.to_string(), url))))?;
        if n == 0 {
            return Ok(size);
        }
        if size + n as u64 > room {
            return Err(Failure::Refused(format!("the server sent more than {max_total} bytes")));
        }
        sink.write_all(&buf[..n]).map_err(|e| Failure::Refused(format!("write: {e}")))?;
        size += n as u64;
    }
}

pub fn sha256_file(path: &std::path::Path) -> std::io::Result<(u64, String)> {
    let mut f = File::open(path)?;
    let (mut h, mut n, mut buf) = (Sha256::new(), 0u64, vec![0u8; 65536]);
    loop {
        let k = f.read(&mut buf)?;
        if k == 0 {
            return Ok((n, crate::util::hex(&h.finalize())));
        }
        h.update(&buf[..k]);
        n += k as u64;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_refused_base_names_the_url_without_its_credentials_or_query() {
        let url = "http://user:s3cret-token@example.com/nevr-runtime/x?token=T";
        let mut sink = Vec::new();
        let Err(Failure::Refused(msg)) = fetch_capped(url, 0, 10, &mut sink) else { panic!("an http base that is not loopback is refused") };
        assert!(!msg.contains("s3cret-token") && !msg.contains("user:") && !msg.contains("token=T"), "{msg}");
        assert!(msg.contains("http://example.com/nevr-runtime/x"), "{msg}");
    }

    #[test]
    fn a_missing_local_base_names_the_path_without_its_query() {
        let mut sink = Vec::new();
        let Err(Failure::Refused(msg)) = fetch_capped("file:///definitely/not/here/x?token=T", 0, 10, &mut sink) else { panic!("a missing file is refused") };
        assert!(!msg.contains("token=T"), "{msg}");
    }
}
