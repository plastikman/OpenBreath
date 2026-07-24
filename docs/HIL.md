# Hardware-in-the-loop testing

DragonBreath HIL runs the production policy state machine on either a GPIO-safe
ESP32-C3 development board or a real Panda Breath. `tools/hil.py` builds the
selected profile, optionally flashes it, drives line-delimited JSON commands,
captures the complete serial console, evaluates scripted assertions, and writes
a machine-readable report.

## Qualification status

As of 2026-07-23:

- The complete devboard suite passes on a dual-USB-C ESP32-C3 board through its
  CH341-family UART bridge.
- Both devboard transports pass the compile-time GPIO/ADC exclusion audit.
- The native-USB image builds and flashes, but this board's application-side
  native USB serial endpoint did not respond during qualification.
- The real-Panda UART profile passes the non-heating smoke scenario on physical
  hardware, both as a guarded build/flash run and as an already-flashed run.

## Safety boundaries

The two HIL targets intentionally have different hardware behavior.

### Devboard target

`CONFIG_PB_HIL_DEVBOARD` replaces or removes every Panda-specific physical
backend used by the test path:

- relay output
- fan TRIAC gate and zero-cross GPIO
- chamber and PTC ADC
- Panda indicator LEDs
- board-strap input

Heater demand, heater output, fan percentage, LED patterns, and zero-cross
diagnostics still update as logical state. Every response identifies this target
with:

```json
{
  "state": {
    "io": {
      "target": "devboard",
      "mains_gpio_compiled_out": true
    }
  }
}
```

`tests/check_hil_compileout.sh` inspects the built component archives and fails
if a devboard backend references the excluded GPIO or ADC APIs.

### Real-Panda target

The Panda profile uses real sensors and actuators. The runner provides two
independent host-side acknowledgements:

- `--allow-panda-flash` authorizes replacing firmware on a real unit.
- `--allow-heater` authorizes positive-target `power_on`, `auto`, or `drying`
  commands.

These checks protect runner-driven workflows; they are not authentication in
the firmware protocol. A different serial writer can send commands directly.
Real-unit HIL must therefore be supervised with the normal electrical and
thermal precautions.

The Panda power LED must remain disabled in this profile. GPIO21 is both the
optional power LED and UART0 TX:

```text
# CONFIG_PB_POWER_LED is not set
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
```

## Profiles

| Target | Transport | Overlay | Typical port | Physical I/O |
|---|---|---|---|---|
| Devboard | Native USB Serial/JTAG | `sdkconfig.hil-devboard` | `/dev/ttyACM0` | Panda I/O compiled out |
| Devboard | USB-to-UART bridge | `sdkconfig.hil-devboard-uart` | `/dev/ttyUSB0` | Panda I/O compiled out |
| Panda | On-board UART bridge | `sdkconfig.hil-panda` | `/dev/ttyUSB0` | Real hardware active |

When `--transport` is omitted, the runner infers `native` from `/dev/ttyACM*`
and `uart` from `/dev/ttyUSB*`. Pass it explicitly for other device naming
schemes.

Each profile uses a separate generated `sdkconfig.profile` inside its build
directory. This prevents HIL symbols or console choices from leaking into the
normal firmware build.

## Prerequisites

On the build machine:

- this repository and its submodules
- ESP-IDF 5.3
- Python from the ESP-IDF environment
- `pyserial` for local serial I/O
- `ssh` and `scp` for remote operation

On a remote device host:

- SSH access
- Python 3.9 or newer
- `pyserial`
- an esptool installation when using `--flash`
- permission to open the serial device, normally through the `dialout` group

Activate ESP-IDF before a build or any `--flash` invocation:

```bash
source ~/esp/esp-idf/export.sh
idf.py --version
python -c 'import serial; print(serial.__version__)'
```

## Local operation

Run all scenarios compatible with a bridge-connected devboard:

```bash
python tools/hil.py \
  --target devboard \
  --port /dev/ttyUSB0 \
  --flash \
  --suite
```

Run one scenario without rebuilding or flashing:

```bash
python tools/hil.py \
  --target devboard \
  --port /dev/ttyUSB0 \
  --scenario tests/hil/scenarios/devboard-safety.json
```

Use native USB explicitly:

```bash
python tools/hil.py \
  --target devboard \
  --transport native \
  --port /dev/ttyACM0 \
  --flash \
  --suite
```

Drive the console interactively by entering one JSON object per line:

```bash
python tools/hil.py \
  --target devboard \
  --port /dev/ttyUSB0 \
  --interactive
```

Example interactive input:

```json
{"cmd":"sensor","channel":"chamber","status":"ok","temp_c":25}
{"cmd":"sensor","channel":"ptc","status":"ok","temp_c":25}
{"cmd":"mode","mode":"power_on","target_c":50}
{"cmd":"state"}
{"cmd":"off"}
```

## Remote device-host operation

Use `--host` when ESP-IDF and the repository are on the workstation but the
serial device is attached to another machine. The runner:

1. builds locally when `--flash` is present;
2. reads ESP-IDF's generated `flasher_args.json`;
3. creates an isolated directory on the remote host;
4. transfers the exact flash images, runner, and scenarios;
5. flashes and runs beside the serial device;
6. copies `report.json` and `serial.jsonl` back to the local output directory.

For a bridge-connected board on a Linux device host:

```bash
python tools/hil.py \
  --host tester@hil-host \
  --target devboard \
  --port /dev/ttyUSB0 \
  --remote-esptool /home/tester/hil-tools/.venv/bin/esptool.py \
  --flash \
  --suite \
  --verbose
```

To rerun the suite against the already-flashed firmware:

```bash
python tools/hil.py \
  --host tester@hil-host \
  --target devboard \
  --port /dev/ttyUSB0 \
  --suite
```

Useful remote options:

| Option | Default | Purpose |
|---|---|---|
| `--remote-python` | `python3` | Python interpreter containing `pyserial` |
| `--remote-esptool` | `esptool.py` | Remote esptool executable |
| `--remote-dir` | `/tmp/dragonbreath-hil` | Remote staging root |
| `--flash-baud` | `460800` | Esptool transfer baud |
| `--output-dir` | timestamped `hil-results/` | Local report destination |

Remote operation requires an explicit `--port`; local auto-detection is not
used for a different machine.

## Real-Panda qualification

Run the non-heating smoke scenario against an already-flashed unit:

```bash
python tools/hil.py \
  --host tester@hil-host \
  --target panda \
  --port /dev/ttyUSB0 \
  --scenario tests/hil/scenarios/panda-smoke.json
```

Authorize a real-unit flash separately:

```bash
python tools/hil.py \
  --host tester@hil-host \
  --target panda \
  --port /dev/ttyUSB0 \
  --remote-esptool /home/tester/hil-tools/.venv/bin/esptool.py \
  --flash \
  --allow-panda-flash \
  --scenario tests/hil/scenarios/panda-smoke.json
```

Add `--allow-heater` only for a supervised scenario that intentionally requests
a positive temperature. The provided Panda smoke scenario never requests heat.

### Reference hardware result

The real-Panda workflow was qualified on 2026-07-23 using commit `7883dda`, an
ESP32-C3 revision 1.1 Panda connected to a generic Linux SSH device host through
its on-board QinHeng `1a86:7522` UART bridge, and ESP-IDF 5.3.1 on the build
machine.

Before flashing, the complete 4 MiB device flash was read and its local and
remote SHA-256 digests were compared. The guarded run then:

1. built the `sdkconfig.hil-panda` profile;
2. transferred the manifest and images to the device host;
3. flashed and reset the Panda with remote esptool;
4. ran `panda-smoke.json`; and
5. retrieved `report.json` and `serial.jsonl`.

The same scenario passed again without `--flash`, proving the normal attach and
test path against an already-running unit. Both runs reported:

- `target: "panda"` over the `uart` transport;
- `mains_gpio_compiled_out: false`, confirming the real hardware profile;
- valid chamber/PTC readings and live zero-cross counts;
- successful `state` and unconditional `off` commands; and
- final mode `off` with `heater_demand: false` and `heater_output: false`.

The build profile contained `CONFIG_PB_HIL_CONSOLE=y`,
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, and no `CONFIG_PB_POWER_LED`. Neither run
used `--allow-heater`, so the harness could not issue a positive heat request.

## Manual builds

The runner performs these isolated builds when `--flash` is supplied. They can
also be run directly:

```bash
idf.py -B build-hil-devboard \
  -DSDKCONFIG="$PWD/build-hil-devboard/sdkconfig.profile" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hil-devboard" \
  build

idf.py -B build-hil-devboard-uart \
  -DSDKCONFIG="$PWD/build-hil-devboard-uart/sdkconfig.profile" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hil-devboard-uart" \
  build

idf.py -B build-hil-panda \
  -DSDKCONFIG="$PWD/build-hil-panda/sdkconfig.profile" \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hil-panda" \
  build
```

Audit either devboard build:

```bash
sh tests/check_hil_compileout.sh build-hil-devboard
sh tests/check_hil_compileout.sh build-hil-devboard-uart
```

## Scenario format

Scenarios are JSON objects stored under `tests/hil/scenarios/`:

```json
{
  "name": "example",
  "targets": ["devboard"],
  "steps": [
    {
      "name": "inject a nominal sensor",
      "send": {
        "cmd": "sensor",
        "channel": "chamber",
        "status": "ok",
        "temp_c": 25
      },
      "expect": {
        "ok": true,
        "state.sensors.chamber.status": "ok"
      }
    },
    {
      "name": "request heat and save the lease",
      "send": {"cmd": "mode", "mode": "power_on", "target_c": 50},
      "save": {"lease": "lease_id"}
    },
    {"name": "allow one control tick", "wait_s": 0.7},
    {
      "name": "renew ownership",
      "send": {"cmd": "heartbeat", "lease_id": "$lease"},
      "expect": {"ok": true, "state.lease_active": true}
    }
  ]
}
```

Step fields:

| Field | Meaning |
|---|---|
| `name` | Human-readable step label |
| `send` | JSON request object; the runner adds `id` |
| `wait_s` | Delay-only step, mutually exclusive with `send` |
| `timeout_s` | Response timeout override, default 4 seconds |
| `expect` | Dotted response paths and expected values |
| `save` | Variable name to dotted response path |

A string beginning with `$` substitutes a saved variable recursively in later
requests. Assertions accept exact values or one operator:

```json
{
  "state.io.zero_cross_count": {"gte": 12},
  "state.mode": {"ne": "drying"},
  "state.fault_reason": {"contains": "sensor"}
}
```

Supported operators are `eq`, `ne`, `gt`, `gte`, `lt`, `lte`, and `contains`.
`--suite` loads only scenarios whose `targets` include the selected target.

## Serial protocol

Requests are one UTF-8 JSON object per line. Firmware emits exactly one
correlated response prefixed with `DBHIL `:

```text
{"cmd":"state","id":7}
DBHIL {"id":7,"ok":true,"state":{...}}
```

Other serial lines are treated as ordinary ESP-IDF console logs and retained in
the transcript.

Commands available on both targets:

| Command | Required fields | Effect |
|---|---|---|
| `ping` | none | Return current state |
| `state` | none | Return current state |
| `off` | none | Request authoritative OFF |
| `mode` | `mode` and mode-specific values | Select `off`, `power_on`, `auto`, or `drying` |
| `heartbeat` | `lease_id` | Renew active remote ownership |
| `clear_fault` | none | Clear a recoverable fault and remain OFF |

Devboard-only injection:

| Command | Fields | Effect |
|---|---|---|
| `sensor` | `channel`, `status`, optional `temp_c` | Inject chamber/PTC value or `open`, `short`, `uninit` |
| `env` | `connected`, `bed_c` | Inject Moonraker connection and bed temperature |
| `zero_cross` | optional `count`, `interval_us` | Inject fan zero-cross diagnostics |
| `button` | `button`, `level` | Inject a raw level (`0` pressed, `1` released) for `power`, `auto`, `on`, or `dry` |

Every response includes the authoritative policy snapshot: mode, source,
revision, targets, heater demand/output, fan percentage, fault/inhibit state,
lease state, sensors, printer environment, logical LEDs, target identity, and
zero-cross diagnostics.

## Results

Each scripted run writes:

- `report.json`: target, transport, port, overall result, scenario results,
  per-step duration, response snapshots, and failures.
- `serial.jsonl`: timestamped `tx` and `rx` records containing every command,
  response, and console log line.

The default local location is `hil-results/YYYYMMDD-HHMMSS/`. This directory is
gitignored. With `--host`, results are generated remotely and then copied to the
same local structure. The downloaded report records the SSH host and rewrites
its transcript path to the local copy. Missing remote artifacts fail the run
instead of being ignored.

## CI coverage

The firmware workflow:

- runs the policy, API, dashboard, and HIL runner tests;
- builds normal firmware with ESP-IDF 5.3;
- builds native-USB and UART devboard HIL images;
- runs the compile-out audit against both images;
- uploads both devboard application binaries.

Physical HIL remains a deliberate lab qualification rather than a GitHub-hosted
runner job.

## Troubleshooting

### No serial device

Inspect USB enumeration and persistent names:

```bash
lsusb
ls -l /dev/serial/by-id
ls -l /dev/ttyACM* /dev/ttyUSB*
dmesg --ctime | tail -30
```

Native ESP USB normally appears as `/dev/ttyACM*`; CH341/CH340 bridges normally
appear as `/dev/ttyUSB*`.

### Permission denied

Check the device group and current memberships:

```bash
ls -l /dev/ttyUSB0
id
```

Add the device-host user to `dialout`, then start a new login session.

### Port opens but requests time out

- Confirm the firmware transport matches the connector.
- Confirm no monitor or other process owns the port with
  `fuser -v /dev/ttyUSB0`.
- Reset the board and inspect the captured ESP-IDF logs.
- If using native USB, try the bridge/UART profile; native application serial
  remains unqualified on the current dual-USB-C board.

The runner explicitly releases DTR/RTS after opening the port and bounds serial
writes so an unresponsive native USB endpoint cannot wedge the test process.

### Board remains in the ROM loader

Use the connector matching the selected profile and reset without holding BOOT.
For remote flashing, verify that `--remote-esptool` names the intended
installation and that the user can open the selected port.

### Wrong configuration appears in a build

Do not share the repository-root generated `sdkconfig` between profiles. Use the
documented `-DSDKCONFIG=.../sdkconfig.profile` commands or let the runner select
the isolated build directory.

### Real Panda UART output is missing

Verify `CONFIG_PB_POWER_LED` is disabled. Enabling it claims GPIO21 and conflicts
with UART0 TX.
