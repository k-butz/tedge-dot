//! Fuzz the contract-level configuration parser: arbitrary text must never panic, only parse
//! or fail. Connector configs are edited by hand and patched remotely via management
//! commands, so hostile/corrupt input is a normal operating condition.
//!
//! Point-library resolution (§3.4) runs over the same input: it walks arbitrary
//! `points_from` entries and joins them onto a base directory, so it must be total over
//! nonsense references (empty strings, `..`, absolute paths, non-UTF-8-ish names) too.

#![no_main]

use libfuzzer_sys::fuzz_target;
use tedge_dot_sdk::config::{parse_duration, ConnectorConfig};

fuzz_target!(|data: &[u8]| {
    if let Ok(text) = std::str::from_utf8(data) {
        let _ = toml::from_str::<ConnectorConfig>(text);
        let _ = parse_duration(text);
        // A directory that does not exist: no library reference can resolve, which exercises
        // reference parsing and the not-found paths without depending on the host's files.
        let base = std::env::temp_dir().join("tedge-dot-fuzz-no-such-dir");
        let _ = tedge_dot_sdk::library::resolve(text, &base);
    }
});
