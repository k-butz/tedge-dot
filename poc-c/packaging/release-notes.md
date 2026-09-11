Experimental **C proof-of-concept** build of tedge-dot — all five OT connectors
(Modbus, OPC UA, CAN bus, CANopen and **PROFIBUS-DP**, which the Rust package
omits) in one small native binary, around 330 KB.

### Try it

Download the package for your architecture from the assets below, then:

```sh
# Debian / Ubuntu
sudo apt-get install -y ./tedge-dot-c_*_amd64.deb
sudo systemctl status tedge-dot

# RPM distros
sudo dnf install ./tedge-dot-c-*.x86_64.rpm

# Alpine
sudo apk add --allow-untrusted ./tedge-dot-c_*_x86_64.apk
```

Or just grab a tarball and run the binary directly — it doubles as a one-shot
read/write CLI:

```sh
./tedge-dot read -c config-defaults/modbus.toml --json
```

`.deb`, `.rpm`, `.apk` and tarballs are attached for **amd64**, **arm64** and
**armhf**. `SHA256SUMS` covers every asset.

### Notes

- Cross-compiled with zig against a **glibc 2.17** floor, so these install on
  distros as old as Debian 8 / RHEL 7 — not only on recent ones.
- Runtime dependencies (Debian/Ubuntu names): `libmodbus5`, `libmosquitto1`,
  `libcjson1`. open62541 is statically linked.
- The packages are a **drop-in replacement** for the Rust `tedge-dot` package
  (same binary path, systemd unit and config layout), so they conflict with it
  — install one or the other.
- On a fresh install the service starts with no devices configured. Add
  `[[device]]` sections under `/etc/tedge/plugins/ot/`, or copy a demo config
  from `/usr/share/tedge-dot/demo/`, then restart the service.
- See `poc-c/README.md` for scope, caveats and what has been verified against
  real simulators.
