# Point libraries

A **point library** is the data point list of one *device type*, in its own
file, with no connection information in it (contract
[§3.4](../../doc/contract/ot-connector-contract.md#34-point-libraries),
[RFC 0004](../../doc/rfc/0004-point-libraries.md)). Device instances reference
it and supply only their own address:

```toml
[[device]]
name             = "plc-7"
protocol_address = { transport = "tcp", host = "192.168.0.17", port = 502, unit_id = 1 }
points_from      = ["pymodbus-demo"]
```

Libraries are looked up by name under `<dir>/<protocol>/<name>.toml`, in each
directory of the search path, first match winning:

| Directory | For |
| --- | --- |
| `/etc/tedge/plugins/ot/points.d` | a site's own libraries; shadow packaged ones of the same name |
| `/usr/share/tedge-dot/points.d` | libraries shipped by a package (this directory, once installed) |

Override the path with `[connector] point_library_path` or, when running from a
source checkout, `TEDGE_DOT_POINT_LIBRARY_PATH` (colon-separated).
