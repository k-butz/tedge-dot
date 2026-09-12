tedge-dot ships as **two interchangeable packages**, each containing the same
`/usr/bin/tedge-dot` binary, the same systemd unit and the same
`/etc/tedge/plugins/ot/` config layout. They speak the same
[OT Connector Contract](https://github.com/thin-edge/tedge-dot/blob/main/doc/contract/),
so a config, a flow or a cloud integration built against one works unchanged against
the other. Install **one or the other** — they declare each other as conflicting.

| Package | Implementation | Pick it when |
|---|---|---|
| `tedge-dot-rs` | Rust (`impl/rust/`) | Default. Richest protocol support, one static binary, no shared-library dependencies. |
| `tedge-dot-c` | C (`impl/c/`) | Small or old devices: ~25x smaller, a **glibc 2.17** floor (Debian 8 / RHEL 7 era), and it additionally ships the **PROFIBUS-DP** connector. |

### Install

Download the package for your architecture from the assets below, then:

```sh
# Debian / Ubuntu
sudo apt-get install -y ./tedge-dot-rs_*_linux_amd64.deb   # or ./tedge-dot-c_*_amd64.deb
sudo systemctl status tedge-dot

# RPM distros
sudo dnf install ./tedge-dot-rs-*.x86_64.rpm

# Alpine
sudo apk add --allow-untrusted ./tedge-dot-rs_*_x86_64.apk
```

Or grab a tarball and run the binary directly — it doubles as a one-shot
read/write CLI:

```sh
./tedge-dot read -c config-defaults/modbus.toml --json
```

`SHA256SUMS` covers every asset in this release.

### Notes

- On a fresh install the service starts with **no devices configured**. Add
  `[[device]]` sections under `/etc/tedge/plugins/ot/`, or copy a demo config
  from `/usr/share/tedge-dot/demo/`, then restart the service.
- The connector is a dumb driver: the device-side **flows** that map its
  envelopes onto the thin-edge data model are deployed active into
  `/etc/tedge/mappers/c8y/flows/`, with the opt-in alarm/event flows in
  `/usr/share/tedge-dot/flows/`. See `flows/README.md`.
- `tedge-dot-c` runtime dependencies (Debian/Ubuntu names): `libmodbus5`,
  `libmosquitto1`, `libcjson1`; open62541 is statically linked. It is
  cross-compiled with zig against the glibc 2.17 floor.
- `tedge-dot-rs` omits the PROFIBUS connector: its serial dependency has a
  native libudev build script that does not cross-compile. Build from source on
  Linux with `cargo build --features profibus`, or use `tedge-dot-c`.
- Where the two implementations differ in behaviour, see the parity table in
  `impl/c/README.md`.
