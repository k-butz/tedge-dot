//! Contract-level configuration model (protocol-neutral). The protocol-specific objects
//! (`connection`, `device.protocol_address`, `point.address`) are kept as raw JSON values and
//! parsed by the connector module in `configure`.

use crate::model::{DataType, Mode, Transform};
use serde::Deserialize;
use std::time::Duration;

#[derive(Debug, Clone, Deserialize)]
pub struct ConnectorConfig {
    pub connector: ConnectorSection,
    #[serde(default)]
    pub mqtt: MqttSection,
    /// Protocol-specific shared connection defaults (opaque to the contract).
    #[serde(default)]
    pub connection: serde_json::Value,
    #[serde(rename = "device", default)]
    pub devices: Vec<DeviceConfig>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ConnectorSection {
    pub protocol: String,
    #[serde(default = "default_service_name")]
    pub service_name: String,
    #[serde(default = "default_poll_interval")]
    pub poll_interval: String,
    #[serde(default = "default_log_level")]
    pub log_level: String,
    /// Upper bound on a single protocol-module call (read batch, write, connect, subscribe).
    /// A module that never returns — what a half-open TCP socket produces: no answer, no
    /// error, no RST — would otherwise block the connector's loop forever, stopping samples,
    /// health and link status with nothing logged. The runtime turns the bound into an
    /// ordinary transport error, so the usual degraded-link and reconnect handling applies.
    #[serde(default = "default_operation_timeout")]
    pub operation_timeout: String,
    /// How long the connector's loop may make no progress before it is considered wedged and
    /// restarted (the supervisor cancels and re-runs it; the MQTT last will marks the service
    /// down so the cloud sees the outage). `"0"` disables the watchdog.
    #[serde(default = "default_stall_timeout")]
    pub stall_timeout: String,
    /// Directories searched for the point libraries devices name in `points_from`
    /// ([`crate::library`]). Unset means the built-in path: the site directory
    /// `/etc/tedge/plugins/ot/points.d` first, then the packaged
    /// `/usr/share/tedge-dot/points.d`. Relative entries resolve against the configuration
    /// file's own directory.
    #[serde(default)]
    pub point_library_path: Option<Vec<String>>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MqttSection {
    #[serde(default = "default_mqtt_host")]
    pub host: String,
    #[serde(default = "default_mqtt_port")]
    pub port: u16,
}

impl Default for MqttSection {
    fn default() -> Self {
        MqttSection {
            host: default_mqtt_host(),
            port: default_mqtt_port(),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct DeviceConfig {
    pub name: String,
    /// Protocol-specific device address (opaque to the contract).
    pub protocol_address: serde_json::Value,
    #[serde(default)]
    pub poll_interval: Option<String>,
    #[serde(default)]
    pub default_mode: Option<Mode>,
    /// Point libraries this device inherits its points from, in order (§3.4). Names are
    /// resolved against the library search path; entries containing `/` or ending in `.toml`
    /// are paths, relative ones against the configuration file's directory. Resolution
    /// happens in [`crate::library`] when the configuration is loaded, so by the time a
    /// connector sees this config `points` already holds the fully-resolved list and this
    /// field is only a record of where it came from.
    #[serde(default)]
    pub points_from: Vec<String>,
    #[serde(rename = "point", default)]
    pub points: Vec<PointConfig>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct PointConfig {
    pub id: String,
    #[serde(default)]
    pub mode: Option<Mode>,
    #[serde(default)]
    pub datatype: Option<DataType>,
    #[serde(default)]
    pub endianness: Option<String>,
    #[serde(default)]
    pub word_order: Option<String>,
    #[serde(default)]
    pub poll_interval: Option<String>,
    /// Protocol-specific point address (opaque to the contract).
    pub address: serde_json::Value,
    #[serde(default)]
    pub access: Option<String>,
    #[serde(default)]
    pub unit: Option<String>,
    /// Short human-readable label for this signal, for where a name is displayed instead of the
    /// `id` (which is a topic segment and a fragment key, so it stays a plain identifier).
    /// Feeds a parameter's DTM title and the `point_labels` of the capability descriptor (§7).
    #[serde(default)]
    pub name: Option<String>,
    /// Longer human-readable explanation of what this signal is. Feeds a parameter's DTM
    /// description and the `point_labels` of the capability descriptor (§7). Neither this nor
    /// `name` is echoed per sample: they are static, so they are published once, retained.
    #[serde(default)]
    pub description: Option<String>,
    /// Optional per-point linear transform applied by the connector after decode.
    #[serde(default)]
    pub transform: Option<Transform>,
    /// Free-form signal metadata, echoed verbatim as `meta` in every sample envelope for this
    /// point. Flows read it for per-signal behaviour (e.g. `on_change`, `min_interval`,
    /// `deadband`); the connector and runtime never interpret it.
    #[serde(default)]
    pub meta: Option<serde_json::Value>,
    /// Set to `false` to keep this point on the polling schedule even when the connector
    /// supports push delivery (`subscribe`). Defaults to push when available.
    #[serde(default)]
    pub subscribe: Option<bool>,
}

impl PointConfig {
    /// Resolve the effective output mode, given the device default.
    pub fn resolved_mode(&self, device_default: Option<Mode>) -> Mode {
        self.mode.or(device_default).unwrap_or(Mode::Typed)
    }
}

fn default_service_name() -> String {
    "tedge-dot".to_string()
}
fn default_poll_interval() -> String {
    "2s".to_string()
}
fn default_log_level() -> String {
    "info".to_string()
}
fn default_operation_timeout() -> String {
    "30s".to_string()
}
fn default_stall_timeout() -> String {
    "120s".to_string()
}
fn default_mqtt_host() -> String {
    "127.0.0.1".to_string()
}
fn default_mqtt_port() -> u16 {
    1883
}

/// Parse a thin-edge duration string (`"500ms"`, `"2s"`, `"5m"`). Falls back to seconds for a
/// bare number. Negative, NaN and overflowing values yield `None` — config values arrive from
/// hand-edited files and remote `set-config` commands, so this must never panic.
pub fn parse_duration(s: &str) -> Option<Duration> {
    let s = s.trim();
    if let Some(rest) = s.strip_suffix("ms") {
        return rest.trim().parse::<u64>().ok().map(Duration::from_millis);
    }
    let (rest, scale) = if let Some(rest) = s.strip_suffix('s') {
        (rest, 1.0)
    } else if let Some(rest) = s.strip_suffix('m') {
        (rest, 60.0)
    } else if let Some(rest) = s.strip_suffix('h') {
        (rest, 3600.0)
    } else {
        (s, 1.0)
    };
    let secs = rest.trim().parse::<f64>().ok()? * scale;
    Duration::try_from_secs_f64(secs).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timeout_defaults_are_sane_and_overridable() {
        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "modbus"
"#,
        )
        .unwrap();
        assert_eq!(parse_duration(&cfg.connector.operation_timeout), Some(Duration::from_secs(30)));
        assert_eq!(parse_duration(&cfg.connector.stall_timeout), Some(Duration::from_secs(120)));

        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "modbus"
operation_timeout = "3s"
stall_timeout = "0"
"#,
        )
        .unwrap();
        assert_eq!(parse_duration(&cfg.connector.operation_timeout), Some(Duration::from_secs(3)));
        // "0" disables the watchdog
        assert_eq!(parse_duration(&cfg.connector.stall_timeout), Some(Duration::ZERO));
    }

    #[test]
    fn durations() {
        assert_eq!(parse_duration("500ms"), Some(Duration::from_millis(500)));
        assert_eq!(parse_duration("2s"), Some(Duration::from_secs(2)));
        assert_eq!(parse_duration("5m"), Some(Duration::from_secs(300)));
        assert_eq!(parse_duration("2h"), Some(Duration::from_secs(7200)));
        assert_eq!(parse_duration("3"), Some(Duration::from_secs(3)));
    }

    /// Found by the `config_toml` fuzz target: negative/NaN/overflowing durations used to
    /// panic in `Duration::from_secs_f64`.
    #[test]
    fn invalid_durations_are_none_not_panics() {
        assert_eq!(parse_duration("-66"), None);
        assert_eq!(parse_duration("-5s"), None);
        assert_eq!(parse_duration("NaN"), None);
        assert_eq!(parse_duration("inf"), None);
        assert_eq!(parse_duration("1e300h"), None);
        assert_eq!(parse_duration(""), None);
        assert_eq!(parse_duration("abc"), None);
    }
}
