//! The SDK runtime: wraps a [`Connector`] and delivers all contract behaviour (MQTT,
//! scheduling, command routing, capability descriptor, health & link status) so protocol
//! modules stay tiny.

use crate::config::{parse_duration, ConnectorConfig};
use crate::connector::{
    Access, Capabilities, CommandRequest, Connector, ConnectorError, LinkReport, LinkStatus,
    PointRef, SampleSink,
};
use crate::decode::{Endianness, WordOrder};
use crate::model::{format_rfc3339_ms, Mode, Sample};
use rumqttc::{AsyncClient, Event, LastWill, MqttOptions, Packet, QoS};
use std::collections::{HashMap, HashSet};
use std::future::Future;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use time::OffsetDateTime;
use toml_edit::{ArrayOfTables, DocumentMut, InlineTable, Item, Table, Value as EditValue};
use tracing::{debug, error, info, warn};

type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// Liveness marker for a connector's loop, shared with whoever supervises it.
///
/// The loop stamps it on every iteration; a supervisor that sees it go stale knows the loop is
/// wedged (a protocol call that never returns) and can cancel and restart the connector — the
/// loop itself cannot do that, since the hang is *inside* it. Monotonic: it measures elapsed
/// time from a shared start instant, so a wall-clock change cannot make a live loop look stuck.
#[derive(Clone, Debug)]
pub struct Progress(Arc<(Instant, AtomicU64)>);

impl Progress {
    pub fn new() -> Self {
        let progress = Progress(Arc::new((Instant::now(), AtomicU64::new(0))));
        progress.mark();
        progress
    }

    /// Record that the loop just made progress.
    pub fn mark(&self) {
        let elapsed = self.0 .0.elapsed().as_millis() as u64;
        self.0 .1.store(elapsed, Ordering::Relaxed);
    }

    /// How long since the last `mark()`.
    pub fn idle(&self) -> Duration {
        let now = self.0 .0.elapsed().as_millis() as u64;
        Duration::from_millis(now.saturating_sub(self.0 .1.load(Ordering::Relaxed)))
    }
}

impl Default for Progress {
    fn default() -> Self {
        Progress::new()
    }
}

/// Bounds the runtime puts on the protocol module. Carried to the few helpers that call it.
#[derive(Clone, Copy, Debug)]
struct Limits {
    /// Upper bound on one protocol-module call (`ConnectorSection::operation_timeout`).
    operation: Duration,
}

impl Limits {
    fn from_config(config: &ConnectorConfig) -> Self {
        let operation = parse_duration(&config.connector.operation_timeout)
            .filter(|d| !d.is_zero())
            .unwrap_or_else(|| {
                warn!(
                    "invalid connector.operation_timeout '{}'; using 30s",
                    config.connector.operation_timeout
                );
                Duration::from_secs(30)
            });
        Limits { operation }
    }
}

/// Run one protocol-module call under the runtime's operation bound.
///
/// A module that hangs instead of failing (a half-open socket answers nothing and never resets)
/// would otherwise block the connector's whole loop: no samples, no health, no link status, and
/// nothing logged, because every one of those is published from that loop. Turning the hang into
/// a transport error lets the existing degraded-link and reconnect-with-backoff handling run.
async fn bounded<T>(
    limits: Limits,
    what: &str,
    call: impl Future<Output = Result<T, ConnectorError>>,
) -> Result<T, ConnectorError> {
    match tokio::time::timeout(limits.operation, call).await {
        Ok(result) => result,
        Err(_) => Err(ConnectorError::Transport(format!(
            "{what} did not return within {}s (operation_timeout)",
            limits.operation.as_secs()
        ))),
    }
}

/// Tracks the last published link status per device so the runtime can publish the
/// contract-required transitions: `degraded` when a whole poll batch fails (e.g. the device
/// dropped mid-run), back to `connected` when reads recover. A device whose initial connect
/// failed stays `disconnected` — failing reads add no information there.
struct LinkTracker {
    protocol: String,
    states: HashMap<String, LinkStatus>,
    /// Last device descriptor seen per device, re-attached to transition reports so the
    /// retained link message keeps carrying it.
    infos: HashMap<String, serde_json::Value>,
}

impl LinkTracker {
    fn new(protocol: &str) -> Self {
        LinkTracker {
            protocol: protocol.to_string(),
            states: HashMap::new(),
            infos: HashMap::new(),
        }
    }

    /// Publish connector-produced link reports (from `connect`) and record their status.
    async fn publish_reports(
        &mut self,
        client: &AsyncClient,
        reports: &[LinkReport],
    ) -> Result<(), BoxError> {
        for report in reports {
            self.states.insert(report.device.clone(), report.status);
            if let Some(info) = &report.info {
                self.infos.insert(report.device.clone(), info.clone());
            }
        }
        publish_links(client, &self.protocol, reports).await
    }

    /// Record a device descriptor without publishing, so a later transition publish carries
    /// it (used when a reconnect succeeded but the link waits for reads to confirm).
    fn stash_info(&mut self, device: &str, info: Option<serde_json::Value>) {
        if let Some(info) = info {
            self.infos.insert(device.to_string(), info);
        }
    }

    /// Publish a link report only when it changes the recorded status — reconnect attempts
    /// repeat on a backoff schedule and must not re-publish the same retained status.
    async fn publish_if_changed(&mut self, client: &AsyncClient, report: &LinkReport) {
        if self.states.get(&report.device) == Some(&report.status) {
            return;
        }
        if let Err(e) = self
            .publish_reports(client, std::slice::from_ref(report))
            .await
        {
            warn!(device = %report.device, "failed to publish link transition: {e}");
        }
    }

    /// Record the outcome of one poll batch for `device` (`healthy` = at least one point was
    /// readable) and publish a retained link transition when the status changed.
    async fn note_poll(
        &mut self,
        client: &AsyncClient,
        device: &str,
        healthy: bool,
        reason: Option<String>,
    ) {
        let current = self.states.get(device).copied();
        let Some(new) = next_link_state(current, healthy) else {
            return;
        };
        info!(%device, status = new.as_str(), "link status changed");
        let report = LinkReport {
            device: device.to_string(),
            status: new,
            reason,
            info: self.infos.get(device).cloned(),
        };
        if let Err(e) = self.publish_reports(client, std::slice::from_ref(&report)).await {
            warn!(%device, "failed to publish link transition: {e}");
        }
    }
}

/// Reconnect backoff bounds: first retry after one second, doubling to a one-minute cap.
const RECONNECT_INITIAL: Duration = Duration::from_secs(1);
const RECONNECT_MAX: Duration = Duration::from_secs(60);

/// The delay to wait after a reconnect attempt that did not restore data flow. Pure so the
/// schedule is unit-testable.
fn next_backoff(current: Duration) -> Duration {
    current.saturating_mul(2).min(RECONNECT_MAX)
}

/// One device pending transport recovery: entries are created when a whole poll batch fails,
/// re-armed after every reconnect attempt, and removed only once reads succeed again.
struct ReconnectEntry {
    delay: Duration,
    due: Instant,
}

impl ReconnectEntry {
    fn new() -> Self {
        ReconnectEntry {
            delay: RECONNECT_INITIAL,
            due: Instant::now() + RECONNECT_INITIAL,
        }
    }

    fn re_arm(&mut self) {
        self.delay = next_backoff(self.delay);
        self.due = Instant::now() + self.delay;
    }
}

/// Try to re-establish one unhealthy device. Prefers the connector's per-device
/// [`Connector::reconnect`]; falls back to a full [`Connector::connect`] when unsupported.
///
/// A successful transport reconnect is deliberately NOT published as `connected`: an
/// application-level outage keeps the transport connectable while reads still fail, and
/// publishing `connected` here would make the retained link status flap. The next healthy
/// poll batch publishes the `connected` transition (with the stashed device descriptor);
/// failed attempts publish `disconnected` once via `publish_if_changed`.
/// Returns true when the transport is back, so the caller can re-arm anything that died with
/// the old one (push subscriptions).
async fn attempt_reconnect(
    connector: &mut Box<dyn Connector>,
    client: &AsyncClient,
    links: &mut LinkTracker,
    device: &str,
    limits: Limits,
) -> bool {
    debug!(%device, "attempting reconnect");
    let reports: Vec<LinkReport> = match bounded(
        limits,
        "reconnect",
        connector.reconnect(&device.to_string()),
    )
    .await
    {
        Ok(report) => vec![report],
        Err(ConnectorError::Unsupported(_)) => match bounded(limits, "connect", connector.connect()).await {
            Ok(reports) => reports,
            Err(e) => {
                warn!(%device, "reconnect (full connect) failed: {e}");
                return false;
            }
        },
        Err(e) => {
            warn!(%device, "reconnect failed: {e}");
            return false;
        }
    };
    let mut restored = false;
    for report in reports {
        if report.status == LinkStatus::Connected {
            if report.device == device {
                restored = true;
            }
            links.stash_info(&report.device, report.info.clone());
        } else {
            links.publish_if_changed(client, &report).await;
        }
    }
    restored
}

/// The link state to publish after a poll batch, or `None` when nothing changed. Pure so the
/// transition rules are unit-testable.
fn next_link_state(current: Option<LinkStatus>, healthy: bool) -> Option<LinkStatus> {
    if healthy {
        (current != Some(LinkStatus::Connected)).then_some(LinkStatus::Connected)
    } else {
        match current {
            // never connected: stay disconnected rather than "upgrade" to degraded
            Some(LinkStatus::Disconnected) | None => None,
            Some(LinkStatus::Degraded) => None,
            Some(LinkStatus::Connected) => Some(LinkStatus::Degraded),
        }
    }
}

/// A single scheduled read job for one point on one device.
struct ScheduleEntry {
    device_index: usize,
    point: PointRef,
    interval: Duration,
    next_due: Instant,
}

/// Run the connector under the SDK runtime until the process receives Ctrl-C or SIGTERM.
///
/// `config_path` is the file the typed `config` was loaded from; the runtime keeps the raw
/// document so management commands (§6.3) can patch and persist it.
pub async fn run(
    connector: Box<dyn Connector>,
    config: ConnectorConfig,
    config_path: PathBuf,
) -> Result<(), BoxError> {
    run_until(connector, config, config_path, shutdown_signal()).await
}

/// Resolve when the process is asked to stop: Ctrl-C (all platforms) or SIGTERM (unix, what
/// systemd sends on `systemctl stop`).
pub async fn shutdown_signal() {
    #[cfg(unix)]
    {
        use tokio::signal::unix::{signal, SignalKind};
        let mut term = match signal(SignalKind::terminate()) {
            Ok(s) => s,
            Err(_) => {
                let _ = tokio::signal::ctrl_c().await;
                return;
            }
        };
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {}
            _ = term.recv() => {}
        }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }
}

/// Run the connector under the SDK runtime until `shutdown` resolves.
///
/// This is the composable variant of [`run`]: a host binary that runs several connectors in
/// one process passes each instance the same shutdown trigger and supervises them itself.
pub async fn run_until(
    connector: Box<dyn Connector>,
    config: ConnectorConfig,
    config_path: PathBuf,
    shutdown: impl std::future::Future<Output = ()> + Send,
) -> Result<(), BoxError> {
    run_until_watched(connector, config, config_path, shutdown, Progress::new()).await
}

/// Same as [`run_until`], but stamping `progress` on every loop iteration so a supervisor can
/// tell a wedged connector from a quiet one and restart it (see [`Progress`]).
pub async fn run_until_watched(
    mut connector: Box<dyn Connector>,
    mut config: ConnectorConfig,
    config_path: PathBuf,
    shutdown: impl std::future::Future<Output = ()> + Send,
    progress: Progress,
) -> Result<(), BoxError> {
    let limits = Limits::from_config(&config);
    let protocol = config.connector.protocol.clone();
    let service = config.connector.service_name.clone();

    // Keep the raw configuration document so management commands can patch & persist it
    // (preserving comments/formatting via toml_edit).
    let mut config_doc: DocumentMut = std::fs::read_to_string(&config_path)
        .ok()
        .and_then(|t| t.parse::<DocumentMut>().ok())
        .unwrap_or_default();

    // 1. Configure the protocol module with the parsed config.
    connector
        .configure(&config)
        .map_err(|e| format!("configure failed: {e}"))?;
    let mut caps = connector.capabilities();
    augment_management_caps(&mut caps);
    augment_batch_caps(&mut caps);

    // 2. MQTT setup.
    let health_topic = format!("te/device/main/service/{service}/status/health");
    let cap_topic = format!("te/device/main/service/{service}/ot/capabilities");
    let cmd_sub = format!("te/device/+/ot/{protocol}/cmd/+/+");

    let mut opts = MqttOptions::new(
        format!("{service}-{protocol}"),
        config.mqtt.host.clone(),
        config.mqtt.port,
    );
    opts.set_keep_alive(Duration::from_secs(30));
    let down_payload = serde_json::json!({
        "status": "down",
        "time": format_rfc3339_ms(OffsetDateTime::now_utc())
    })
    .to_string();
    opts.set_last_will(LastWill::new(
        health_topic.clone(),
        down_payload,
        QoS::AtLeastOnce,
        true,
    ));

    let (client, mut eventloop) = AsyncClient::new(opts, 32);

    // Drive the MQTT event loop from its own task, forwarding incoming publishes to the main
    // loop. The event loop MUST NOT share a select loop with publishing: while the broker is
    // unreachable the client's request queue fills, `publish().await` then blocks the shared
    // loop, the event loop stops being polled, and the connector wedges permanently — even
    // after the broker comes back. (Observed when the connector service started before the
    // broker.) A dedicated task keeps draining the queue no matter what the main loop awaits.
    let (incoming_tx, mut incoming_rx) = tokio::sync::mpsc::channel::<rumqttc::Publish>(32);
    tokio::spawn(async move {
        loop {
            match eventloop.poll().await {
                Ok(Event::Incoming(Packet::Publish(p))) => {
                    if incoming_tx.send(p).await.is_err() {
                        break; // runtime shut down
                    }
                }
                Ok(_) => {}
                // The client half was dropped and every queued request (including the final
                // health "down") has been flushed: this runtime instance is gone, so the
                // task must exit rather than retry — the process may host other connectors
                // and restart this one, and leaked event loops would pile up.
                Err(rumqttc::ConnectionError::RequestsDone) => break,
                Err(e) => {
                    warn!("mqtt event loop error: {e}; retrying");
                    tokio::time::sleep(Duration::from_secs(1)).await;
                }
            }
        }
    });

    // 3. Publish capability descriptor + service health (retained).
    publish_retained(&client, &cap_topic, caps.to_json().to_string()).await?;
    publish_health(&client, &health_topic, "up").await?;
    client.subscribe(&cmd_sub, QoS::AtLeastOnce).await?;
    info!(%protocol, %service, "connector started");

    // 4. Connect to devices and publish link status.
    let mut links = LinkTracker::new(&protocol);
    match bounded(limits, "connect", connector.connect()).await {
        Ok(reports) => links.publish_reports(&client, &reports).await?,
        Err(e) => warn!("initial connect failed: {e}"),
    }
    // 5. Set up push delivery for subscribe-capable connectors, then build the polling
    // schedule for everything that is not pushed. The runtime keeps `sample_tx` alive for
    // the whole run so re-subscribing after a config reload reuses the same channel.
    let (sample_tx, mut sample_rx) = tokio::sync::mpsc::channel::<Sample>(256);
    let mut subscribed =
        setup_subscriptions(&mut connector, &config, caps.subscribe, &sample_tx, limits).await;
    let mut schedule = build_schedule(&config, &subscribed);
    let mut meta_index = build_meta_index(&config);
    let mut seq_counters: HashMap<(String, String), u64> = HashMap::new();
    // Devices whose transport needs re-establishing, keyed by device name.
    let mut reconnects: HashMap<String, ReconnectEntry> = HashMap::new();

    // 6. Main loop: poll due points on a tick, route commands from the MQTT event-loop task.
    let mut tick = tokio::time::interval(Duration::from_millis(200));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    tokio::pin!(shutdown);

    loop {
        tokio::select! {
            _ = &mut shutdown => {
                info!("shutdown requested");
                break;
            }
            _ = tick.tick() => {
                let now = Instant::now();
                // Gather due points grouped by device.
                let mut due: HashMap<usize, Vec<PointRef>> = HashMap::new();
                for entry in schedule.iter_mut() {
                    if entry.next_due <= now {
                        due.entry(entry.device_index).or_default().push(entry.point.clone());
                        entry.next_due = now + entry.interval;
                    }
                }
                for (device_index, points) in due {
                    let device = config.devices[device_index].name.clone();
                    match bounded(limits, "read", connector.read_points(&device, &points)).await {
                        Ok(mut samples) => {
                            for s in samples.iter_mut() {
                                // The runtime owns the device identity for polled reads:
                                // connectors routinely leave `device` empty, and the sample
                                // topic + meta lookup are keyed by the configured name.
                                s.device = device.clone();
                                publish_sample(&client, &protocol, s, &mut seq_counters, &meta_index)
                                    .await;
                            }
                            // A batch where every point failed means the device itself is
                            // unreachable (a single bad point keeps the link healthy).
                            if !samples.is_empty() {
                                let healthy =
                                    samples.iter().any(|s| s.quality != crate::model::Quality::Bad);
                                let reason = (!healthy)
                                    .then(|| samples.iter().find_map(|s| s.error.clone()))
                                    .flatten();
                                links.note_poll(&client, &device, healthy, reason).await;
                                if healthy {
                                    reconnects.remove(&device);
                                } else {
                                    reconnects.entry(device.clone()).or_insert_with(ReconnectEntry::new);
                                }
                            }
                        }
                        Err(e) => {
                            warn!(%device, "read_points failed: {e}");
                            links.note_poll(&client, &device, false, Some(e.to_string())).await;
                            reconnects.entry(device.clone()).or_insert_with(ReconnectEntry::new);
                        }
                    }
                }
                // The loop completed an iteration: samples published, reconnects attempted.
                // A supervisor watching this marker restarts the connector if it stops moving.
                progress.mark();
                // Re-establish unhealthy devices on their backoff schedule. Entries stay
                // until reads succeed: a transport that reconnects while the device still
                // fails (application-level outage) keeps backing off instead of storming.
                let now = Instant::now();
                let due: Vec<String> = reconnects
                    .iter()
                    .filter(|(_, entry)| entry.due <= now)
                    .map(|(device, _)| device.clone())
                    .collect();
                for device in due {
                    let restored =
                        attempt_reconnect(&mut connector, &client, &mut links, &device, limits)
                            .await;
                    if let Some(entry) = reconnects.get_mut(&device) {
                        entry.re_arm();
                    }
                    // A push subscription dies with the transport it was created on. Without
                    // re-arming it here the device's subscribed points stay OFF the polling
                    // schedule with nothing delivering them -- silent for good, behind a link
                    // that recovers to `connected` on the next healthy poll.
                    if restored && caps.subscribe {
                        if let Some((device_index, device_config)) = config
                            .devices
                            .iter()
                            .enumerate()
                            .find(|(_, d)| d.name == device)
                        {
                            subscribe_device(
                                &mut connector,
                                &config,
                                device_index,
                                device_config,
                                &sample_tx,
                                limits,
                                &mut subscribed,
                            )
                            .await;
                            schedule = build_schedule(&config, &subscribed);
                        }
                    }
                }
            }
            Some(mut sample) = sample_rx.recv() => {
                publish_sample(&client, &protocol, &mut sample, &mut seq_counters, &meta_index)
                    .await;
                progress.mark();
            }
            Some(p) = incoming_rx.recv() => {
                match handle_command(
                    &mut connector, &client, &protocol, &mut links,
                    &mut config, &mut config_doc, &config_path,
                    &p.topic, &p.payload, limits,
                ).await {
                    // A management command changed the config: re-establish push
                    // delivery (the reload disconnected the old subscriptions) and
                    // rebuild the polling schedule.
                    Ok(true) => {
                        subscribed = setup_subscriptions(
                            &mut connector, &config, caps.subscribe, &sample_tx, limits,
                        ).await;
                        schedule = build_schedule(&config, &subscribed);
                        meta_index = build_meta_index(&config);
                        seq_counters.clear();
                        // the management path already reconnected every device
                        reconnects.clear();
                    }
                    Ok(false) => {}
                    Err(e) => warn!("command handling error: {e}"),
                }
                progress.mark();
            }
        }
    }

    // 7. Clean shutdown.
    let _ = bounded(limits, "disconnect", connector.disconnect()).await;
    publish_health(&client, &health_topic, "down").await.ok();
    Ok(())
}

/// Run the connector without a broker until `shutdown` resolves: every sample is printed to
/// stdout as one JSON envelope per line (NDJSON) instead of being published over MQTT. The
/// envelope carries the source `device`, so interleaved output from several connectors (or
/// devices) stays identifiable.
///
/// This powers `tedge-dot run --output stdout` for local exploration and piping into other
/// tools. It reuses the exact scheduling, subscription, seq and meta handling of the MQTT
/// runtime; link transitions are logged via tracing, and the broker-borne features (health,
/// capabilities, commands/management) are simply absent.
pub async fn run_stdout_until(
    mut connector: Box<dyn Connector>,
    config: ConnectorConfig,
    shutdown: impl std::future::Future<Output = ()> + Send,
) -> Result<(), BoxError> {
    let limits = Limits::from_config(&config);
    connector
        .configure(&config)
        .map_err(|e| format!("configure failed: {e}"))?;
    let caps = connector.capabilities();

    match bounded(limits, "connect", connector.connect()).await {
        Ok(reports) => {
            for report in &reports {
                info!(device = %report.device, status = report.status.as_str(),
                    reason = report.reason.as_deref().unwrap_or(""), "link");
            }
        }
        Err(e) => warn!("initial connect failed: {e}"),
    }

    let (sample_tx, mut sample_rx) = tokio::sync::mpsc::channel::<Sample>(256);
    let subscribed =
        setup_subscriptions(&mut connector, &config, caps.subscribe, &sample_tx, limits).await;
    let mut schedule = build_schedule(&config, &subscribed);
    let meta_index = build_meta_index(&config);
    let mut seq_counters: HashMap<(String, String), u64> = HashMap::new();
    let mut reconnects: HashMap<String, ReconnectEntry> = HashMap::new();

    let mut tick = tokio::time::interval(Duration::from_millis(200));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    tokio::pin!(shutdown);

    loop {
        tokio::select! {
            _ = &mut shutdown => break,
            _ = tick.tick() => {
                let now = Instant::now();
                let mut due: HashMap<usize, Vec<PointRef>> = HashMap::new();
                for entry in schedule.iter_mut() {
                    if entry.next_due <= now {
                        due.entry(entry.device_index).or_default().push(entry.point.clone());
                        entry.next_due = now + entry.interval;
                    }
                }
                for (device_index, points) in due {
                    let device = config.devices[device_index].name.clone();
                    match bounded(limits, "read", connector.read_points(&device, &points)).await {
                        Ok(mut samples) => {
                            for s in samples.iter_mut() {
                                s.device = device.clone();
                                print_sample(s, &mut seq_counters, &meta_index);
                            }
                            let healthy = samples.is_empty()
                                || samples.iter().any(|s| s.quality != crate::model::Quality::Bad);
                            if healthy {
                                reconnects.remove(&device);
                            } else {
                                reconnects.entry(device.clone()).or_insert_with(ReconnectEntry::new);
                            }
                        }
                        Err(e) => {
                            warn!(%device, "read_points failed: {e}");
                            reconnects.entry(device.clone()).or_insert_with(ReconnectEntry::new);
                        }
                    }
                }
                let now = Instant::now();
                let due: Vec<String> = reconnects
                    .iter()
                    .filter(|(_, entry)| entry.due <= now)
                    .map(|(device, _)| device.clone())
                    .collect();
                for device in due {
                    debug!(%device, "attempting reconnect");
                    match connector.reconnect(&device).await {
                        Ok(_) => {}
                        Err(ConnectorError::Unsupported(_)) => {
                            if let Err(e) = connector.connect().await {
                                warn!(%device, "reconnect (full connect) failed: {e}");
                            }
                        }
                        Err(e) => warn!(%device, "reconnect failed: {e}"),
                    }
                    if let Some(entry) = reconnects.get_mut(&device) {
                        entry.re_arm();
                    }
                }
            }
            Some(mut sample) = sample_rx.recv() => {
                print_sample(&mut sample, &mut seq_counters, &meta_index);
            }
        }
    }

    let _ = bounded(limits, "disconnect", connector.disconnect()).await;
    Ok(())
}

/// Stamp the per-point sequence number and print one sample envelope to stdout (one JSON
/// object per line); the stdout counterpart of [`publish_sample`].
fn print_sample(
    sample: &mut Sample,
    seq_counters: &mut HashMap<(String, String), u64>,
    meta_index: &MetaIndex,
) {
    let counter = seq_counters
        .entry((sample.device.clone(), sample.point.clone()))
        .or_insert(0);
    *counter += 1;
    sample.seq = Some(*counter);
    println!("{}", envelope_with_meta(sample, meta_index));
}

/// Build the polling schedule, skipping points that are delivered by subscription.
fn build_schedule(
    config: &ConnectorConfig,
    subscribed: &HashSet<(usize, String)>,
) -> Vec<ScheduleEntry> {
    let connector_default = parse_duration(&config.connector.poll_interval)
        .unwrap_or_else(|| Duration::from_secs(2));
    let now = Instant::now();
    let mut schedule = Vec::new();
    for (device_index, device) in config.devices.iter().enumerate() {
        let device_default = device
            .poll_interval
            .as_deref()
            .and_then(parse_duration)
            .unwrap_or(connector_default);
        for point in &device.points {
            if subscribed.contains(&(device_index, point.id.clone())) {
                continue;
            }
            let interval = point
                .poll_interval
                .as_deref()
                .and_then(parse_duration)
                .unwrap_or(device_default);
            let mut point = point_ref(point, device.default_mode);
            point.interval = Some(interval);
            schedule.push(ScheduleEntry {
                device_index,
                point,
                interval,
                next_due: now,
            });
        }
    }
    schedule
}

/// Ask a subscribe-capable connector for push delivery, device by device. Points configured
/// with `subscribe = false` are excluded and stay on the polling schedule, as does every point
/// of a device whose `subscribe()` call does not succeed. Returns the set of
/// `(device_index, point_id)` now delivered via push.
async fn setup_subscriptions(
    connector: &mut Box<dyn Connector>,
    config: &ConnectorConfig,
    subscribe_capable: bool,
    sink: &SampleSink,
    limits: Limits,
) -> HashSet<(usize, String)> {
    let mut subscribed = HashSet::new();
    if !subscribe_capable {
        return subscribed;
    }
    for (device_index, device) in config.devices.iter().enumerate() {
        subscribe_device(
            connector,
            config,
            device_index,
            device,
            sink,
            limits,
            &mut subscribed,
        )
        .await;
    }
    subscribed
}

/// Arm push delivery for ONE device, recording its points in `subscribed` on success and
/// removing them on failure (so they fall back to the polling schedule).
///
/// Separate from [`setup_subscriptions`] because a device that reconnects has to be
/// re-subscribed on its own: its monitored items died with the old session, while its
/// siblings' are still live and must not be created twice.
async fn subscribe_device(
    connector: &mut Box<dyn Connector>,
    config: &ConnectorConfig,
    device_index: usize,
    device: &crate::config::DeviceConfig,
    sink: &SampleSink,
    limits: Limits,
    subscribed: &mut HashSet<(usize, String)>,
) {
    let connector_default = parse_duration(&config.connector.poll_interval)
        .unwrap_or_else(|| Duration::from_secs(2));
    {
        let device_default = device
            .poll_interval
            .as_deref()
            .and_then(parse_duration)
            .unwrap_or(connector_default);
        let points: Vec<PointRef> = device
            .points
            .iter()
            .filter(|p| p.subscribe.unwrap_or(true))
            .map(|p| {
                let mut r = point_ref(p, device.default_mode);
                r.interval = Some(
                    p.poll_interval
                        .as_deref()
                        .and_then(parse_duration)
                        .unwrap_or(device_default),
                );
                r
            })
            .collect();
        if points.is_empty() {
            return;
        }
        // Drop any stale entries first: on a re-subscribe these points are currently marked
        // as pushed, and if the call below fails they must go back to being polled rather
        // than stay off the schedule with no subscription behind them.
        for p in &points {
            subscribed.remove(&(device_index, p.id.clone()));
        }
        match bounded(
            limits,
            "subscribe",
            connector.subscribe(&device.name, &points, sink.clone()),
        )
        .await
        {
            Ok(()) => {
                info!(device = %device.name, points = points.len(), "subscribed (push delivery)");
                for p in &points {
                    subscribed.insert((device_index, p.id.clone()));
                }
            }
            Err(ConnectorError::Unsupported(_)) => {
                debug!(device = %device.name, "subscribe unsupported; polling");
            }
            Err(e) => {
                warn!(device = %device.name, "subscribe failed: {e}; falling back to polling");
            }
        }
    }
}

/// Per-point `meta` lookup, keyed by `(device name, point id)`; injected into every published
/// sample envelope so flows can apply per-signal behaviour without their own config.
/// Per-point configuration echoed into every sample envelope beyond what the driver produces:
/// the free-form `meta` table and the declared `access` (so consumers can tell writable
/// points — parameters — apart without the configuration file).
#[derive(Clone, Debug)]
struct PointExtras {
    meta: Option<serde_json::Value>,
    access: Access,
}

type MetaIndex = HashMap<(String, String), PointExtras>;

fn build_meta_index(config: &ConnectorConfig) -> MetaIndex {
    let mut index = HashMap::new();
    for device in &config.devices {
        for point in &device.points {
            index.insert(
                (device.name.clone(), point.id.clone()),
                PointExtras {
                    meta: point.meta.clone(),
                    access: Access::parse(point.access.as_deref()),
                },
            );
        }
    }
    index
}

fn access_str(access: Access) -> &'static str {
    match access {
        Access::Read => "read",
        Access::Write => "write",
        Access::ReadWrite => "read_write",
    }
}

/// The sample envelope as published: the contract envelope plus the point's `meta` (if any)
/// and its `access`.
fn envelope_with_meta(sample: &Sample, meta_index: &MetaIndex) -> serde_json::Value {
    let mut envelope = sample.to_envelope();
    if let Some(extras) = meta_index.get(&(sample.device.clone(), sample.point.clone())) {
        if let Some(meta) = &extras.meta {
            envelope["meta"] = meta.clone();
        }
        envelope["access"] = serde_json::Value::String(access_str(extras.access).into());
    }
    envelope
}

/// Stamp the per-point sequence number and publish one sample. Shared by the polling loop and
/// the subscription channel so both paths get identical seq/meta/topic handling.
async fn publish_sample(
    client: &AsyncClient,
    protocol: &str,
    sample: &mut Sample,
    seq_counters: &mut HashMap<(String, String), u64>,
    meta_index: &MetaIndex,
) {
    let counter = seq_counters
        .entry((sample.device.clone(), sample.point.clone()))
        .or_insert(0);
    *counter += 1;
    sample.seq = Some(*counter);
    let topic = format!(
        "te/device/{}/ot/{}/sample/{}",
        sample.device, protocol, sample.point
    );
    let payload = envelope_with_meta(sample, meta_index).to_string();
    if let Err(e) = client.publish(&topic, QoS::AtMostOnce, false, payload).await {
        error!("failed to publish sample: {e}");
    }
}

/// Build a resolved [`PointRef`] from a configured point. Shared by the scheduler and by callers
/// (e.g. a CLI) that drive a connector's `read_points`/`execute` directly.
pub fn point_ref(point: &crate::config::PointConfig, device_default: Option<Mode>) -> PointRef {
    PointRef {
        id: point.id.clone(),
        mode: point.resolved_mode(device_default),
        datatype: point.datatype,
        endianness: Endianness::parse(point.endianness.as_deref()),
        word_order: WordOrder::parse(point.word_order.as_deref()),
        access: Access::parse(point.access.as_deref()),
        unit: point.unit.clone(),
        transform: point.transform.unwrap_or_default(),
        interval: point.poll_interval.as_deref().and_then(parse_duration),
    }
}

#[allow(clippy::too_many_arguments)]
async fn handle_command(
    connector: &mut Box<dyn Connector>,
    client: &AsyncClient,
    protocol: &str,
    links: &mut LinkTracker,
    config: &mut ConnectorConfig,
    config_doc: &mut DocumentMut,
    config_path: &Path,
    topic: &str,
    payload: &[u8],
    limits: Limits,
) -> Result<bool, BoxError> {    // Expect te/device/<device>/ot/<protocol>/cmd/<verb>/<id>
    let parts: Vec<&str> = topic.split('/').collect();
    if parts.len() != 8
        || parts[0] != "te"
        || parts[1] != "device"
        || parts[3] != "ot"
        || parts[4] != protocol
        || parts[5] != "cmd"
    {
        return Ok(false);
    }
    let device = parts[2].to_string();
    let verb = parts[6];

    let json: serde_json::Value = match serde_json::from_slice(payload) {
        Ok(v) => v,
        Err(_) => return Ok(false), // empty/clearing message or junk
    };
    let status = json.get("status").and_then(|s| s.as_str()).unwrap_or("");
    if status != "init" {
        return Ok(false); // only act on new requests; ignore our own transitions
    }

    // Management verbs (§6.3) are handled generically by the runtime; they mutate and persist
    // the connector configuration, then live-reload the protocol module.
    if is_management_verb(verb) {
        return handle_management(
            connector, client, links, config, config_doc, config_path, topic, verb, &json, limits,
        )
        .await;
    }

    // `write-batch` (§6.4) is implemented once here on top of the module's `write`.
    if verb == "write-batch" {
        handle_write_batch(connector, client, topic, &device, &json, limits).await?;
        debug!(%device, %verb, "command handled");
        return Ok(false);
    }

    let point = json
        .get("point")
        .and_then(|p| p.as_str())
        .unwrap_or_default()
        .to_string();
    let request = CommandRequest {
        point: point.clone(),
        value: json.get("value").cloned(),
        value_repr: json
            .get("value_repr")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string()),
        raw: json.get("raw").and_then(|v| v.as_str()).map(|s| s.to_string()),
    };

    // executing
    publish_retained(
        client,
        topic,
        serde_json::json!({ "status": "executing", "point": point }).to_string(),
    )
    .await?;

    match bounded(limits, "write", connector.execute(&device, verb, &request)).await {
        Ok(result) => {
            let mut obj = serde_json::Map::new();
            obj.insert("status".into(), serde_json::Value::String("successful".into()));
            obj.insert("point".into(), serde_json::Value::String(result.point));
            if let Some(v) = result.value {
                obj.insert("value".into(), v);
            }
            if let Some(r) = result.raw {
                obj.insert("raw".into(), serde_json::Value::String(r));
            }
            publish_retained(client, topic, serde_json::Value::Object(obj).to_string()).await?;
        }
        // `UnknownPoint` means THIS connector doesn't have `point` configured for `device` — not
        // necessarily that the command is malformed — see `is_foreign_batch_point`'s own doc
        // comment for the full reasoning (this single-write path can't reuse that helper
        // directly, since there's no partial-batch-progress case here, but the same "another
        // connector process shares this device name" scenario applies).
        Err(ConnectorError::UnknownPoint { .. }) => {
            debug!(%device, %verb, %point, "not this connector's point, ignoring");
            return Ok(false);
        }
        Err(e) => {
            publish_retained(
                client,
                topic,
                serde_json::json!({
                    "status": "failed",
                    "point": point,
                    "reason": e.to_string()
                })
                .to_string(),
            )
            .await?;
        }
    }
    debug!(%device, %verb, "command handled");
    Ok(false)
}

/// Advertise the runtime-provided `write-batch` verb for every module that implements
/// `write` (the runtime executes the batch as a sequence of `write` calls).
fn augment_batch_caps(caps: &mut Capabilities) {
    if caps.command_verbs.iter().any(|v| v == "write")
        && !caps.command_verbs.iter().any(|v| v == "write-batch")
    {
        caps.command_verbs.push("write-batch".to_string());
    }
}

/// One entry of a `write-batch` request.
#[derive(Clone, Debug, PartialEq)]
pub struct BatchWrite {
    pub point: String,
    pub value: Option<serde_json::Value>,
    pub raw: Option<String>,
}

/// Parse the `writes` array of a `write-batch` request (§6.4). Each entry needs a `point`
/// and either a `value` (typed write) or `raw` (hex bytes); an empty batch is rejected so a
/// malformed request cannot "succeed" without touching the device.
pub fn parse_batch_writes(json: &serde_json::Value) -> Result<Vec<BatchWrite>, String> {
    let writes = json
        .get("writes")
        .and_then(|w| w.as_array())
        .ok_or_else(|| "write-batch request needs a `writes` array".to_string())?;
    if writes.is_empty() {
        return Err("write-batch request has no writes".into());
    }
    let mut out = Vec::with_capacity(writes.len());
    for (i, w) in writes.iter().enumerate() {
        let point = w
            .get("point")
            .and_then(|p| p.as_str())
            .filter(|p| !p.is_empty())
            .ok_or_else(|| format!("writes[{i}] has no `point`"))?
            .to_string();
        let value = w.get("value").cloned().filter(|v| !v.is_null());
        let raw = w.get("raw").and_then(|r| r.as_str()).map(String::from);
        if value.is_none() && raw.is_none() {
            return Err(format!("writes[{i}] ({point}) has neither `value` nor `raw`"));
        }
        out.push(BatchWrite { point, value, raw });
    }
    Ok(out)
}

/// Whether an `UnknownPoint` failure partway through a write-batch means "this command wasn't
/// meant for this connector" (silently ignore, publish nothing) rather than a real failure.
///
/// The command topic is not scoped by which connector subscribed to it: every connector for a
/// given protocol subscribes to the same `te/device/+/ot/<protocol>/cmd/+/+` wildcard (see
/// `cmd_sub` below). Two connector *processes* that share one protocol but manage disjoint point
/// sets for the same device name — e.g. one connector configured only for device "sensor-1"'s
/// measurement points, a separate one configured for the same device's read/write parameter
/// points — therefore both receive every command for that device. Publishing `failed` from the
/// connector that doesn't own the point would race the OTHER connector's own `successful` result
/// on the SAME retained topic: Cumulocity's operation state machine does not accept a transition
/// out of a terminal status, so whichever connector's result the mapper observes first as
/// `failed` sticks — the operation shows as permanently failed even though the device ends up
/// with the correct value.
///
/// Only applies before anything in the batch has been written yet (`results_so_far == 0`): an
/// `UnknownPoint` after some earlier point in the SAME batch already succeeded means this
/// connector genuinely does own the device but not every point in the batch — a real,
/// inconsistent request worth surfacing rather than silently discarding a partial write.
fn is_foreign_batch_point(error: &ConnectorError, results_so_far: usize) -> bool {
    results_so_far == 0 && matches!(error, ConnectorError::UnknownPoint { .. })
}

/// Execute a `write-batch`: the writes run sequentially in request order through the
/// module's `write` verb and stop at the first failure (later points are left untouched).
/// The result carries one entry per attempted write so a requester can tell what was
/// applied before a failure.
async fn handle_write_batch(
    connector: &mut Box<dyn Connector>,
    client: &AsyncClient,
    topic: &str,
    device: &str,
    json: &serde_json::Value,
    limits: Limits,
) -> Result<(), BoxError> {
    let writes = match parse_batch_writes(json) {
        Ok(w) => w,
        Err(reason) => {
            publish_retained(
                client,
                topic,
                serde_json::json!({ "status": "failed", "reason": reason, "results": [] })
                    .to_string(),
            )
            .await?;
            return Ok(());
        }
    };
    let points: Vec<&str> = writes.iter().map(|w| w.point.as_str()).collect();
    publish_retained(
        client,
        topic,
        serde_json::json!({ "status": "executing", "points": points }).to_string(),
    )
    .await?;

    let mut results: Vec<serde_json::Value> = Vec::with_capacity(writes.len());
    let mut failure: Option<String> = None;
    for w in &writes {
        let request = CommandRequest {
            point: w.point.clone(),
            value: w.value.clone(),
            value_repr: None,
            raw: w.raw.clone(),
        };
        match bounded(
            limits,
            "write",
            connector.execute(&device.to_string(), "write", &request),
        )
        .await
        {
            Ok(result) => {
                let mut obj = serde_json::Map::new();
                obj.insert("point".into(), serde_json::Value::String(result.point));
                obj.insert("status".into(), serde_json::Value::String("successful".into()));
                if let Some(v) = result.value {
                    obj.insert("value".into(), v);
                }
                if let Some(r) = result.raw {
                    obj.insert("raw".into(), serde_json::Value::String(r));
                }
                results.push(serde_json::Value::Object(obj));
            }
            Err(e) if is_foreign_batch_point(&e, results.len()) => {
                debug!(%device, point = %w.point, "not this connector's point, ignoring batch");
                return Ok(());
            }
            Err(e) => {
                let reason = format!("write to {} failed: {e}", w.point);
                results.push(serde_json::json!({
                    "point": w.point,
                    "status": "failed",
                    "reason": reason,
                }));
                failure = Some(reason);
                break;
            }
        }
    }
    let payload = batch_result(failure, results);
    publish_retained(client, topic, payload.to_string()).await?;
    Ok(())
}

/// Shape the terminal `write-batch` envelope: `successful` with every result, or `failed`
/// with the first failure's reason and the results up to and including it.
pub fn batch_result(failure: Option<String>, results: Vec<serde_json::Value>) -> serde_json::Value {
    match failure {
        None => serde_json::json!({ "status": "successful", "results": results }),
        Some(reason) => serde_json::json!({
            "status": "failed",
            "reason": reason,
            "results": results,
        }),
    }
}

/// The protocol-neutral management verbs the SDK runtime implements for every connector.
fn is_management_verb(verb: &str) -> bool {
    matches!(verb, "set-config" | "define-device" | "remove-device")
}

/// Advertise the SDK-provided management verbs in the connector's capability descriptor.
fn augment_management_caps(caps: &mut Capabilities) {
    for verb in ["set-config", "define-device", "remove-device"] {
        if !caps.command_verbs.iter().any(|v| v == verb) {
            caps.command_verbs.push(verb.to_string());
        }
    }
    if !caps.features.iter().any(|f| f == "management") {
        caps.features.push("management".to_string());
    }
}

/// Handle a management command: patch the config document, validate, persist, and live-reload.
/// Returns `Ok(true)` when the configuration changed (so the caller rebuilds the schedule).
#[allow(clippy::too_many_arguments)]
async fn handle_management(
    connector: &mut Box<dyn Connector>,
    client: &AsyncClient,
    links: &mut LinkTracker,
    config: &mut ConnectorConfig,
    config_doc: &mut DocumentMut,
    config_path: &Path,
    topic: &str,
    verb: &str,
    json: &serde_json::Value,
    limits: Limits,
) -> Result<bool, BoxError> {
    publish_retained(
        client,
        topic,
        serde_json::json!({ "status": "executing" }).to_string(),
    )
    .await?;

    // Build a candidate document and validate it parses into a typed config.
    let candidate = {
        let mut doc = config_doc.clone();
        match apply_management(verb, json, &mut doc) {
            Ok(()) => doc,
            Err(e) => {
                publish_failed(client, topic, &e).await?;
                return Ok(false);
            }
        }
    };
    let new_config: ConnectorConfig = match toml::from_str(&candidate.to_string()) {
        Ok(c) => c,
        Err(e) => {
            publish_failed(client, topic, &format!("resulting config is invalid: {e}")).await?;
            return Ok(false);
        }
    };

    // Validate against the protocol module before committing.
    if let Err(e) = connector.configure(&new_config) {
        let _ = connector.configure(config); // restore previous good state
        publish_failed(client, topic, &format!("configure failed: {e}")).await?;
        return Ok(false);
    }

    // Persist the new document (best effort: the running state is already updated).
    if let Err(e) = persist_config(config_path, &candidate) {
        warn!("failed to persist config to {}: {e}", config_path.display());
    }
    *config_doc = candidate;
    *config = new_config;

    // Reconnect with the new configuration and republish link status.
    let _ = bounded(limits, "disconnect", connector.disconnect()).await;
    match bounded(limits, "connect", connector.connect()).await {
        Ok(reports) => links.publish_reports(client, &reports).await?,
        Err(e) => warn!("reconnect after reconfigure failed: {e}"),
    }

    publish_retained(
        client,
        topic,
        serde_json::json!({ "status": "successful" }).to_string(),
    )
    .await?;
    info!(%verb, "management command applied");
    Ok(true)
}

async fn publish_failed(client: &AsyncClient, topic: &str, reason: &str) -> Result<(), BoxError> {
    warn!("management command failed: {reason}");
    publish_retained(
        client,
        topic,
        serde_json::json!({ "status": "failed", "reason": reason }).to_string(),
    )
    .await
}

/// Dispatch a management verb onto the configuration document.
fn apply_management(
    verb: &str,
    json: &serde_json::Value,
    doc: &mut DocumentMut,
) -> Result<(), String> {
    match verb {
        "set-config" => apply_set_config(json, doc),
        "define-device" => apply_define_device(json, doc),
        "remove-device" => apply_remove_device(json, doc),
        other => Err(format!("unsupported management verb '{other}'")),
    }
}

/// `set-config`: deep-merge `config` into the section named by `target`.
fn apply_set_config(json: &serde_json::Value, doc: &mut DocumentMut) -> Result<(), String> {
    let target = json
        .get("target")
        .and_then(|t| t.as_str())
        .ok_or("set-config requires a 'target'")?;
    let patch = json
        .get("config")
        .and_then(|c| c.as_object())
        .ok_or("set-config requires a 'config' object")?;
    let root = doc.as_table_mut();

    if let Some(name) = target.strip_prefix("device:") {
        let devices = root
            .get_mut("device")
            .and_then(Item::as_array_of_tables_mut)
            .ok_or("no devices are configured")?;
        let table = (0..devices.len())
            .find(|&i| {
                devices
                    .get(i)
                    .and_then(|t| t.get("name"))
                    .and_then(|v| v.as_str())
                    == Some(name)
            })
            .and_then(|i| devices.get_mut(i))
            .ok_or_else(|| format!("device '{name}' not found"))?;
        merge_object_into_table(table, patch)
    } else if matches!(target, "connector" | "mqtt" | "connection") {
        let item = root
            .entry(target)
            .or_insert_with(|| Item::Table(Table::new()));
        let table = item
            .as_table_mut()
            .ok_or_else(|| format!("config section '{target}' is not a table"))?;
        merge_object_into_table(table, patch)
    } else {
        Err(format!(
            "unknown set-config target '{target}' (expected connector, mqtt, connection or device:<name>)"
        ))
    }
}

/// `define-device`: insert or replace a `[[device]]` entry by name.
fn apply_define_device(json: &serde_json::Value, doc: &mut DocumentMut) -> Result<(), String> {
    let device = json
        .get("device")
        .and_then(|d| d.as_object())
        .ok_or("define-device requires a 'device' object")?;
    let name = device
        .get("name")
        .and_then(|n| n.as_str())
        .ok_or("device requires a 'name'")?
        .to_string();
    let new_table = json_object_to_table(device)?;

    let devices = doc
        .as_table_mut()
        .entry("device")
        .or_insert_with(|| Item::ArrayOfTables(ArrayOfTables::new()))
        .as_array_of_tables_mut()
        .ok_or("'device' is not an array of tables")?;

    let existing = (0..devices.len()).find(|&i| {
        devices
            .get(i)
            .and_then(|t| t.get("name"))
            .and_then(|v| v.as_str())
            == Some(name.as_str())
    });
    match existing.and_then(|i| devices.get_mut(i)) {
        Some(slot) => *slot = new_table,
        None => devices.push(new_table),
    }
    Ok(())
}

/// `remove-device`: delete the named `[[device]]` entry.
fn apply_remove_device(json: &serde_json::Value, doc: &mut DocumentMut) -> Result<(), String> {
    let name = json
        .get("device")
        .and_then(|d| d.as_str())
        .ok_or("remove-device requires a 'device' name string")?;
    let devices = doc
        .as_table_mut()
        .get_mut("device")
        .and_then(Item::as_array_of_tables_mut)
        .ok_or("no devices are configured")?;
    let index = (0..devices.len()).find(|&i| {
        devices
            .get(i)
            .and_then(|t| t.get("name"))
            .and_then(|v| v.as_str())
            == Some(name)
    });
    match index {
        Some(i) => {
            devices.remove(i);
            Ok(())
        }
        None => Err(format!("device '{name}' not found")),
    }
}

/// Deep-merge a JSON object into a toml_edit table: nested objects merge into existing standard
/// sub-tables, otherwise (absent / inline / scalar) the key is replaced.
fn merge_object_into_table(
    table: &mut Table,
    patch: &serde_json::Map<String, serde_json::Value>,
) -> Result<(), String> {
    for (key, value) in patch {
        match value {
            serde_json::Value::Object(obj) => match table.get_mut(key) {
                Some(item) if item.is_table() => {
                    merge_object_into_table(item.as_table_mut().unwrap(), obj)?;
                }
                _ => {
                    table.insert(
                        key,
                        Item::Value(EditValue::InlineTable(json_object_to_inline(obj)?)),
                    );
                }
            },
            _ => {
                table.insert(key, Item::Value(json_value_to_edit(value)?));
            }
        }
    }
    Ok(())
}

/// Convert a JSON object into a standard toml_edit table; nested object arrays become
/// arrays-of-tables (e.g. `point`), nested objects become inline tables (e.g. `protocol_address`).
fn json_object_to_table(
    obj: &serde_json::Map<String, serde_json::Value>,
) -> Result<Table, String> {
    let mut table = Table::new();
    for (key, value) in obj {
        match value {
            serde_json::Value::Array(items)
                if !items.is_empty() && items.iter().all(serde_json::Value::is_object) =>
            {
                let mut aot = ArrayOfTables::new();
                for item in items {
                    aot.push(json_object_to_table(item.as_object().unwrap())?);
                }
                table.insert(key, Item::ArrayOfTables(aot));
            }
            _ => {
                table.insert(key, Item::Value(json_value_to_edit(value)?));
            }
        }
    }
    Ok(table)
}

fn json_object_to_inline(
    obj: &serde_json::Map<String, serde_json::Value>,
) -> Result<InlineTable, String> {
    let mut inline = InlineTable::new();
    for (key, value) in obj {
        inline.insert(key, json_value_to_edit(value)?);
    }
    Ok(inline)
}

fn json_value_to_edit(value: &serde_json::Value) -> Result<EditValue, String> {
    Ok(match value {
        serde_json::Value::Null => return Err("null values are not allowed in config".into()),
        serde_json::Value::Bool(b) => EditValue::from(*b),
        serde_json::Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                EditValue::from(i)
            } else if let Some(f) = n.as_f64() {
                EditValue::from(f)
            } else {
                return Err(format!("unsupported number: {n}"));
            }
        }
        serde_json::Value::String(s) => EditValue::from(s.as_str()),
        serde_json::Value::Array(items) => {
            let mut array = toml_edit::Array::new();
            for item in items {
                array.push(json_value_to_edit(item)?);
            }
            EditValue::Array(array)
        }
        serde_json::Value::Object(obj) => EditValue::InlineTable(json_object_to_inline(obj)?),
    })
}

fn persist_config(path: &Path, doc: &DocumentMut) -> Result<(), String> {
    let text = doc.to_string();
    let tmp = path.with_extension("toml.tmp");
    std::fs::write(&tmp, text.as_bytes()).map_err(|e| format!("write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("rename to {}: {e}", path.display()))?;
    Ok(())
}

async fn publish_links(
    client: &AsyncClient,
    protocol: &str,
    reports: &[LinkReport],
) -> Result<(), BoxError> {
    for report in reports {
        let topic = format!("te/device/{}/ot/{}/status/link", report.device, protocol);
        let mut obj = serde_json::Map::new();
        obj.insert(
            "status".into(),
            serde_json::Value::String(report.status.as_str().into()),
        );
        if report.status == LinkStatus::Connected {
            obj.insert(
                "since".into(),
                serde_json::Value::String(format_rfc3339_ms(OffsetDateTime::now_utc())),
            );
        }
        if let Some(reason) = &report.reason {
            obj.insert("reason".into(), serde_json::Value::String(reason.clone()));
        }
        if let Some(info) = &report.info {
            obj.insert("info".into(), info.clone());
        }
        publish_retained(client, &topic, serde_json::Value::Object(obj).to_string()).await?;
    }
    Ok(())
}

async fn publish_health(client: &AsyncClient, topic: &str, status: &str) -> Result<(), BoxError> {
    let payload = serde_json::json!({
        "status": status,
        "time": format_rfc3339_ms(OffsetDateTime::now_utc())
    })
    .to_string();
    publish_retained(client, topic, payload).await
}

async fn publish_retained(
    client: &AsyncClient,
    topic: &str,
    payload: String,
) -> Result<(), BoxError> {
    client
        .publish(topic, QoS::AtLeastOnce, true, payload)
        .await
        .map_err(|e| Box::new(e) as BoxError)
}

/// Resolve the effective output mode of a point ignoring device default; small helper used by
/// modules that want the same logic without the SDK config types.
pub fn resolve_mode(mode: Option<Mode>, device_default: Option<Mode>) -> Mode {
    mode.or(device_default).unwrap_or(Mode::Typed)
}

#[cfg(test)]
mod tests {
    use super::*;

    const BASE: &str = r#"
[connector]
protocol = "modbus"
poll_interval = "2s"
log_level = "info"

[mqtt]
host = "127.0.0.1"
port = 1883

[connection.serial]
baudrate = 9600
parity = "N"
stopbits = 2
databits = 8

[[device]]
name = "plc-1"
protocol_address = { transport = "tcp", host = "127.0.0.1", port = 502, unit_id = 1 }
default_mode = "typed"

  [[device.point]]
  id = "temp"
  datatype = "float32"
  address = { table = "holding", address = 7, count = 2 }
"#;

    fn doc() -> DocumentMut {
        BASE.parse::<DocumentMut>().unwrap()
    }

    /// Apply a verb and return the resulting typed config (asserting it stays valid).
    fn apply(verb: &str, json: serde_json::Value) -> (DocumentMut, ConnectorConfig) {
        let mut d = doc();
        apply_management(verb, &json, &mut d).expect("apply ok");
        let cfg: ConnectorConfig = toml::from_str(&d.to_string()).expect("valid config");
        (d, cfg)
    }

    #[test]
    fn set_config_patches_connector_section() {
        let (_d, cfg) = apply(
            "set-config",
            serde_json::json!({ "target": "connector", "config": { "poll_interval": "5s" } }),
        );
        assert_eq!(cfg.connector.poll_interval, "5s");
        // unrelated fields preserved
        assert_eq!(cfg.connector.log_level, "info");
    }

    #[test]
    fn set_config_deep_merges_serial_defaults() {
        let (d, _cfg) = apply(
            "set-config",
            serde_json::json!({ "target": "connection", "config": { "serial": { "baudrate": 19200 } } }),
        );
        let text = d.to_string();
        assert!(text.contains("baudrate = 19200"), "baudrate patched: {text}");
        // sibling serial keys are retained by the deep merge
        assert!(text.contains("parity"), "parity retained: {text}");
    }

    #[test]
    fn set_config_patches_named_device() {
        let (_d, cfg) = apply(
            "set-config",
            serde_json::json!({ "target": "device:plc-1", "config": { "poll_interval": "10s" } }),
        );
        let dev = cfg.devices.iter().find(|d| d.name == "plc-1").unwrap();
        assert_eq!(dev.poll_interval.as_deref(), Some("10s"));
        // existing points untouched
        assert_eq!(dev.points.len(), 1);
    }

    #[test]
    fn set_config_unknown_target_rejected() {
        let mut d = doc();
        let err = apply_management(
            "set-config",
            &serde_json::json!({ "target": "bogus", "config": {} }),
            &mut d,
        )
        .unwrap_err();
        assert!(err.contains("unknown set-config target"), "{err}");
    }

    #[test]
    fn define_device_appends_new_device() {
        let (_d, cfg) = apply(
            "define-device",
            serde_json::json!({ "device": {
                "name": "plc-9",
                "protocol_address": { "transport": "tcp", "host": "10.0.0.9", "port": 502, "unit_id": 2 },
                "default_mode": "typed",
                "point": [
                    { "id": "level", "datatype": "uint16", "address": { "table": "holding", "address": 1, "count": 1 } }
                ]
            }}),
        );
        assert_eq!(cfg.devices.len(), 2);
        let dev = cfg.devices.iter().find(|d| d.name == "plc-9").unwrap();
        assert_eq!(dev.points.len(), 1);
        assert_eq!(dev.points[0].id, "level");
    }

    #[test]
    fn define_device_replaces_existing_by_name() {
        let (_d, cfg) = apply(
            "define-device",
            serde_json::json!({ "device": {
                "name": "plc-1",
                "protocol_address": { "transport": "tcp", "host": "1.2.3.4", "port": 502, "unit_id": 1 },
                "point": [
                    { "id": "a", "datatype": "int16", "address": { "table": "holding", "address": 0, "count": 1 } },
                    { "id": "b", "datatype": "int16", "address": { "table": "holding", "address": 1, "count": 1 } }
                ]
            }}),
        );
        assert_eq!(cfg.devices.len(), 1, "replaced, not appended");
        assert_eq!(cfg.devices[0].points.len(), 2);
    }

    #[test]
    fn remove_device_deletes_entry() {
        let (_d, cfg) = apply("remove-device", serde_json::json!({ "device": "plc-1" }));
        assert!(cfg.devices.is_empty());
    }

    #[test]
    fn remove_unknown_device_rejected() {
        let mut d = doc();
        let err =
            apply_management("remove-device", &serde_json::json!({ "device": "nope" }), &mut d)
                .unwrap_err();
        assert!(err.contains("not found"), "{err}");
    }

    /// A subscribed point is deliberately OFF the polling schedule -- that is what makes push
    /// delivery push. The corollary is that anything which drops a point from `subscribed`
    /// MUST rebuild the schedule, or the point is delivered by nobody: not polled, and not
    /// pushed either. `subscribe_device` relies on this when it clears a device's entries
    /// before re-arming, so that a failed re-subscribe degrades to polling rather than to
    /// silence.
    #[test]
    fn a_point_is_scheduled_unless_it_is_subscribed() {
        let config: ConnectorConfig = toml::from_str(BASE).unwrap();

        let none = HashSet::new();
        let polled = build_schedule(&config, &none);
        assert_eq!(polled.len(), 1, "an unsubscribed point must be polled");
        assert_eq!(polled[0].point.id, "temp");

        let mut subscribed = HashSet::new();
        subscribed.insert((0usize, "temp".to_string()));
        assert!(
            build_schedule(&config, &subscribed).is_empty(),
            "a subscribed point must not also be polled (it would double-publish)"
        );

        // ...and dropping it from the set puts it straight back on the schedule.
        subscribed.remove(&(0usize, "temp".to_string()));
        assert_eq!(
            build_schedule(&config, &subscribed).len(),
            1,
            "a point that lost its subscription must fall back to polling"
        );
    }

    #[test]
    fn point_meta_parsed_and_indexed() {
        let cfg: ConnectorConfig = toml::from_str(
            r#"
[connector]
protocol = "modbus"

[[device]]
name = "plc-1"
protocol_address = { host = "127.0.0.1" }

  [[device.point]]
  id = "temp"
  datatype = "float32"
  address = { table = "holding", address = 7, count = 2 }
  meta = { on_change = true, min_interval = "5s", room = "boiler" }
"#,
        )
        .unwrap();
        let index = build_meta_index(&cfg);
        let extras = index.get(&("plc-1".to_string(), "temp".to_string())).unwrap();
        let meta = extras.meta.as_ref().unwrap();
        assert_eq!(extras.access, Access::Read);
        assert_eq!(meta["on_change"], serde_json::json!(true));
        assert_eq!(meta["min_interval"], serde_json::json!("5s"));
        assert_eq!(meta["room"], serde_json::json!("boiler"));
    }

    #[test]
    fn envelope_carries_point_meta() {
        let sample = Sample {
            ts: OffsetDateTime::UNIX_EPOCH,
            device: "plc-1".into(),
            protocol: "modbus",
            point: "temp".into(),
            mode: Mode::Typed,
            datatype: None,
            value: None,
            raw: vec![0x12, 0x34],
            raw_group: 2,
            quality: crate::model::Quality::Good,
            unit: None,
            addr: serde_json::Value::Null,
            seq: None,
            error: None,
        };
        let mut index = HashMap::new();
        index.insert(
            ("plc-1".to_string(), "temp".to_string()),
            PointExtras {
                meta: Some(serde_json::json!({ "on_change": true })),
                access: Access::ReadWrite,
            },
        );
        let env = envelope_with_meta(&sample, &index);
        assert_eq!(env["meta"]["on_change"], serde_json::json!(true));
        assert_eq!(env["access"], serde_json::json!("read_write"));
        // a sample of an unindexed point has neither meta nor access
        let env2 = envelope_with_meta(&sample, &HashMap::new());
        assert!(env2.get("meta").is_none());
        assert!(env2.get("access").is_none());
    }

    #[test]
    fn schedule_skips_subscribed_points() {
        let cfg: ConnectorConfig = toml::from_str(BASE).unwrap();
        let none = HashSet::new();
        assert_eq!(build_schedule(&cfg, &none).len(), 1);
        let mut subscribed = HashSet::new();
        subscribed.insert((0usize, "temp".to_string()));
        assert_eq!(build_schedule(&cfg, &subscribed).len(), 0);
    }

    #[test]
    fn schedule_resolves_point_interval() {
        let cfg: ConnectorConfig = toml::from_str(BASE).unwrap();
        let schedule = build_schedule(&cfg, &HashSet::new());
        // connector poll_interval = "2s" flows into the resolved PointRef interval
        assert_eq!(schedule[0].point.interval, Some(Duration::from_secs(2)));
    }

    #[test]
    fn reconnect_backoff_doubles_and_caps() {
        assert_eq!(next_backoff(RECONNECT_INITIAL), Duration::from_secs(2));
        assert_eq!(next_backoff(Duration::from_secs(2)), Duration::from_secs(4));
        assert_eq!(next_backoff(Duration::from_secs(40)), RECONNECT_MAX);
        assert_eq!(next_backoff(RECONNECT_MAX), RECONNECT_MAX);
    }

    #[test]
    fn link_transitions_follow_poll_health() {
        use LinkStatus::*;
        // healthy reads (re)connect from any non-connected state
        assert_eq!(next_link_state(Some(Connected), true), None);
        assert_eq!(next_link_state(Some(Degraded), true), Some(Connected));
        assert_eq!(next_link_state(Some(Disconnected), true), Some(Connected));
        assert_eq!(next_link_state(None, true), Some(Connected));
        // a fully-failing batch degrades a connected link, once
        assert_eq!(next_link_state(Some(Connected), false), Some(Degraded));
        assert_eq!(next_link_state(Some(Degraded), false), None);
        // a device that never connected stays disconnected
        assert_eq!(next_link_state(Some(Disconnected), false), None);
        assert_eq!(next_link_state(None, false), None);
    }

    #[test]
    fn batch_writes_parse_typed_and_raw_entries() {
        let json = serde_json::json!({
            "status": "init",
            "writes": [
                { "point": "setpoint", "value": 21.5 },
                { "point": "mask", "raw": "00ff" }
            ]
        });
        let writes = parse_batch_writes(&json).unwrap();
        assert_eq!(writes.len(), 2);
        assert_eq!(writes[0].point, "setpoint");
        assert_eq!(writes[0].value, Some(serde_json::json!(21.5)));
        assert_eq!(writes[1].raw.as_deref(), Some("00ff"));
        assert_eq!(writes[1].value, None);
    }

    #[test]
    fn batch_writes_reject_malformed_requests() {
        let missing = serde_json::json!({ "status": "init" });
        assert!(parse_batch_writes(&missing).unwrap_err().contains("`writes`"));
        let empty = serde_json::json!({ "writes": [] });
        assert!(parse_batch_writes(&empty).unwrap_err().contains("no writes"));
        let no_point = serde_json::json!({ "writes": [{ "value": 1 }] });
        assert!(parse_batch_writes(&no_point).unwrap_err().contains("`point`"));
        let no_value = serde_json::json!({ "writes": [{ "point": "x" }] });
        assert!(parse_batch_writes(&no_value).unwrap_err().contains("neither"));
        let null_value = serde_json::json!({ "writes": [{ "point": "x", "value": null }] });
        assert!(parse_batch_writes(&null_value).is_err());
    }

    #[test]
    fn batch_result_shapes_success_and_failure() {
        let ok = batch_result(None, vec![serde_json::json!({ "point": "a", "status": "successful" })]);
        assert_eq!(ok["status"], "successful");
        assert_eq!(ok["results"].as_array().unwrap().len(), 1);
        let failed = batch_result(
            Some("write to b failed: boom".into()),
            vec![
                serde_json::json!({ "point": "a", "status": "successful" }),
                serde_json::json!({ "point": "b", "status": "failed", "reason": "write to b failed: boom" }),
            ],
        );
        assert_eq!(failed["status"], "failed");
        assert_eq!(failed["reason"], "write to b failed: boom");
        assert_eq!(failed["results"][1]["status"], "failed");
    }

    #[test]
    fn foreign_batch_point_only_before_any_success() {
        let unknown = ConnectorError::UnknownPoint {
            device: "sensor-1".to_string(),
            point: "threshold".to_string(),
        };
        // Nothing written yet: treat as "not my point", stay silent.
        assert!(is_foreign_batch_point(&unknown, 0));
        // Something in the batch already succeeded: a real, inconsistent failure now.
        assert!(!is_foreign_batch_point(&unknown, 1));
        // Any other error kind is always a real failure, regardless of progress.
        let denied = ConnectorError::AccessDenied("temperature_min".to_string());
        assert!(!is_foreign_batch_point(&denied, 0));
        assert!(!is_foreign_batch_point(&denied, 1));
    }

    #[test]
    fn batch_caps_follow_write_support() {
        let mut caps = Capabilities {
            protocol: "x",
            version: "0",
            modes: vec![],
            datatypes: vec![],
            point_kinds: vec![],
            command_verbs: vec!["write".into()],
            features: vec![],
            subscribe: false,
        };
        augment_batch_caps(&mut caps);
        assert!(caps.command_verbs.iter().any(|v| v == "write-batch"));
        augment_batch_caps(&mut caps); // idempotent
        assert_eq!(caps.command_verbs.iter().filter(|v| *v == "write-batch").count(), 1);
        let mut read_only = caps.clone();
        read_only.command_verbs = vec![];
        augment_batch_caps(&mut read_only);
        assert!(read_only.command_verbs.is_empty());
    }

    #[test]
    fn management_caps_are_advertised() {
        let mut caps = Capabilities {
            protocol: "modbus",
            version: "0.0.0",
            modes: vec![],
            datatypes: vec![],
            point_kinds: vec![],
            command_verbs: vec!["write".into()],
            features: vec!["polling".into()],
            subscribe: false,
        };
        augment_management_caps(&mut caps);
        for verb in ["write", "set-config", "define-device", "remove-device"] {
            assert!(caps.command_verbs.iter().any(|v| v == verb), "missing {verb}");
        }
        assert!(caps.features.iter().any(|f| f == "management"));
    }
}
