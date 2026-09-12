"""Modbus simulator for the tedge-dot e2e harness.

Serves SIM_DEVICES independent Modbus TCP devices (default 1, at most MAX_DEVICES) from one
container, so a stack can run several connector instances that each own a device of their own.

Device N (1-based) listens on port 501+N (502, 503, ...) and serves the register map seeded in
modbus.json, with two differences between devices:

* each device has its own datastore, so a write to one device is not visible on another;
* holding register DEVICE_ID_REGISTER holds the device number, so a test can tell which device a
  connector is really talking to.

Each device also gets the pymodbus simulator's HTTP UI on port 8079+N. The compose healthcheck
probes the first device's (8080), and device 1 is started last, so a healthy container means every
device is serving.
"""

# See also example https://pymodbus.readthedocs.io/en/latest/source/example/updating_server.html
import asyncio
import copy
import json
import logging
import os
import tempfile

from pymodbus.server import ModbusSimulatorServer

BASE_PORT = 502
BASE_HTTP_PORT = 8080
DEVICE_ID_REGISTER = 2100
MAX_DEVICES = 32

logger = logging.getLogger("Modbus Server")
logging.basicConfig(
    level=logging.DEBUG, format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)


def device_setup(setup, number):
    """The simulator setup of device `number`: its own port and its number in the id register."""
    device_setup = copy.deepcopy(setup)
    device_setup["server_list"]["server"]["port"] = BASE_PORT + number - 1
    uint16 = device_setup["device_list"]["device"]["uint16"]
    uint16[:] = [entry for entry in uint16 if entry != DEVICE_ID_REGISTER]
    uint16.append({"addr": DEVICE_ID_REGISTER, "value": number})
    return device_setup


async def run_servers(count):
    """Start `count` simulated devices and serve them until the process is stopped."""
    with open("modbus.json", encoding="utf-8") as file:
        setup = json.load(file)

    # ModbusSimulatorServer only takes its setup from a file, so render one per device.
    workdir = tempfile.mkdtemp(prefix="modbus-sim-")
    servers = []
    for number in reversed(range(1, count + 1)):
        json_file = os.path.join(workdir, f"device-{number}.json")
        with open(json_file, "w", encoding="utf-8") as file:
            json.dump(device_setup(setup, number), file)
        server = ModbusSimulatorServer(
            modbus_server="server",
            modbus_device="device",
            http_port=BASE_HTTP_PORT + number - 1,
            json_file=json_file,
        )
        await server.run_forever(only_start=True)
        servers.append(server)
        logger.info("simulated device %d serving on port %d", number, BASE_PORT + number - 1)

    await asyncio.Event().wait()


def device_count():
    """SIM_DEVICES, validated: a typo must not quietly start a single device."""
    raw = os.environ.get("SIM_DEVICES", "1")
    try:
        count = int(raw)
    except ValueError:
        raise SystemExit(f"SIM_DEVICES must be a number, got {raw!r}")
    if not 1 <= count <= MAX_DEVICES:
        raise SystemExit(f"SIM_DEVICES must be between 1 and {MAX_DEVICES}, got {count}")
    return count


if __name__ == "__main__":
    asyncio.run(run_servers(device_count()), debug=True)
