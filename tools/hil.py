#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build, flash, drive, and report DragonBreath serial HIL scenarios."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import json
import pathlib
import posixpath
import shlex
import subprocess
import sys
import time
from typing import Any, Optional

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCENARIO_DIR = ROOT / "tests" / "hil" / "scenarios"
RESPONSE_PREFIX = "DBHIL "


class HilError(RuntimeError):
    pass


def dotted_get(value: Any, path: str) -> Any:
    current = value
    for part in path.split("."):
        if isinstance(current, dict) and part in current:
            current = current[part]
        else:
            raise HilError(f"response has no field {path!r}")
    return current


def check_expectations(response: dict[str, Any], expected: dict[str, Any]) -> None:
    for path, wanted in expected.items():
        actual = dotted_get(response, path)
        if isinstance(wanted, dict):
            if len(wanted) != 1:
                raise HilError(f"{path}: assertion must contain exactly one operator")
            operator, operand = next(iter(wanted.items()))
            operations = {
                "eq": lambda: actual == operand,
                "ne": lambda: actual != operand,
                "gt": lambda: actual > operand,
                "gte": lambda: actual >= operand,
                "lt": lambda: actual < operand,
                "lte": lambda: actual <= operand,
                "contains": lambda: operand in actual,
            }
            if operator not in operations:
                raise HilError(f"{path}: unknown assertion operator {operator!r}")
            passed = operations[operator]()
        else:
            passed = actual == wanted
        if not passed:
            raise HilError(f"{path}: expected {wanted!r}, got {actual!r}")


def substitute(value: Any, variables: dict[str, Any]) -> Any:
    if isinstance(value, str) and value.startswith("$"):
        name = value[1:]
        if name not in variables:
            raise HilError(f"unknown scenario variable {value}")
        return variables[name]
    if isinstance(value, dict):
        return {key: substitute(item, variables) for key, item in value.items()}
    if isinstance(value, list):
        return [substitute(item, variables) for item in value]
    return value


def requests_heat(command: dict[str, Any]) -> bool:
    if command.get("cmd") != "mode":
        return False
    return command.get("mode") in {"power_on", "auto", "drying"} and (
        float(command.get("target_c", 0)) > 0
    )


def load_scenario(
    path: pathlib.Path, target: Optional[str] = None
) -> dict[str, Any]:
    try:
        scenario = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HilError(f"cannot load scenario {path}: {exc}") from exc
    if not isinstance(scenario, dict) or not isinstance(scenario.get("steps"), list):
        raise HilError(f"{path}: scenario must be an object with a steps array")
    targets = scenario.get("targets", ["devboard", "panda"])
    if target is not None and target not in targets:
        raise HilError(f"{path}: scenario does not support target {target!r}")
    return scenario


def load_suite_scenarios(target: str) -> list[tuple[pathlib.Path, dict[str, Any]]]:
    scenarios = []
    for path in sorted(SCENARIO_DIR.glob("*.json")):
        scenario = load_scenario(path)
        if target in scenario.get("targets", ["devboard", "panda"]):
            scenarios.append((path, scenario))
    if not scenarios:
        raise HilError(f"no suite scenarios support target {target!r}")
    return scenarios


class SerialTransport:
    def __init__(
        self,
        port: str,
        baud: int,
        transcript,
        verbose: bool = False,
    ) -> None:
        try:
            import serial
        except ImportError as exc:
            raise HilError(
                "pyserial is required for HIL I/O; run from the ESP-IDF 5.3 "
                "environment (source ~/esp/esp-idf/export.sh)"
            ) from exc
        self.serial = serial.Serial(
            port=None, baudrate=baud, timeout=0.2, write_timeout=1.0
        )
        # Match ESP-IDF Monitor: establish known levels before opening, then
        # release both lines so native USB does not hold the C3 in reset.
        self.serial.rts = True
        self.serial.dtr = True
        self.serial.port = port
        self.serial.open()
        self.serial.rts = False
        self.serial.dtr = False
        self.transcript = transcript
        self.verbose = verbose
        self.next_id = 1

    def close(self) -> None:
        if sys.platform == "linux" and getattr(self.serial, "is_open", False):
            try:
                import fcntl
                import struct
                import termios

                layout = "iiIiiiiiHcciHHPHIL"
                packed = fcntl.ioctl(
                    self.serial.fd,
                    termios.TIOCGSERIAL,
                    bytes(struct.calcsize(layout)),
                )
                fields = list(struct.unpack(layout, packed))
                fields[12] = 0xFFFF  # ASYNC_CLOSING_WAIT_NONE
                fcntl.ioctl(
                    self.serial.fd, termios.TIOCSSERIAL,
                    struct.pack(layout, *fields),
                )
            except (AttributeError, OSError):
                pass
        try:
            self.serial.cancel_write()
        except (AttributeError, OSError):
            pass
        try:
            self.serial.reset_output_buffer()
        except (AttributeError, OSError):
            pass
        self.serial.close()

    def drain(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self._read_line()

    def _read_line(self) -> Optional[str]:
        raw = self.serial.readline()
        if not raw:
            return None
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        self.transcript.write(
            json.dumps(
                {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "rx": line}
            )
            + "\n"
        )
        self.transcript.flush()
        if self.verbose or not line.startswith(RESPONSE_PREFIX):
            print(f"< {line}")
        return line

    def request(
        self, command: dict[str, Any], timeout_s: float = 4.0
    ) -> dict[str, Any]:
        request = copy.deepcopy(command)
        request_id = self.next_id
        self.next_id += 1
        request["id"] = request_id
        wire = json.dumps(request, separators=(",", ":"))
        self.transcript.write(
            json.dumps(
                {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "tx": wire}
            )
            + "\n"
        )
        self.transcript.flush()
        if self.verbose:
            print(f"> {wire}")
        self.serial.write((wire + "\n").encode())

        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            line = self._read_line()
            if not line or not line.startswith(RESPONSE_PREFIX):
                continue
            try:
                response = json.loads(line[len(RESPONSE_PREFIX) :])
            except json.JSONDecodeError:
                continue
            if response.get("id") == request_id:
                return response
        raise HilError(f"timeout waiting for response to request {request_id}")


def resolve_port(requested: Optional[str]) -> str:
    if requested:
        return requested
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise HilError("pyserial is required to auto-detect a serial port") from exc
    ports = [item.device for item in list_ports.comports()]
    if len(ports) != 1:
        joined = ", ".join(ports) if ports else "none"
        raise HilError(f"pass --port; auto-detection found {len(ports)} ports: {joined}")
    return ports[0]


def resolve_transport(target: str, requested: Optional[str], port: str) -> str:
    if target == "panda":
        if requested == "native":
            raise HilError("Panda HIL uses UART; native USB is not available")
        return "uart"
    if requested:
        return requested
    device_name = pathlib.Path(port).resolve().name
    return "uart" if device_name.startswith("ttyUSB") else "native"


def profile_paths(args) -> tuple[pathlib.Path, pathlib.Path]:
    profile = f"hil-{args.target}"
    if args.target == "devboard" and args.transport == "uart":
        profile += "-uart"
    overlay = ROOT / f"sdkconfig.{profile}"
    build_dir = pathlib.Path(args.build_dir or ROOT / f"build-{profile}")
    return overlay, build_dir


def build_profile(args) -> pathlib.Path:
    overlay, build_dir = profile_paths(args)
    sdkconfig = build_dir / "sdkconfig.profile"
    defaults = f"{ROOT / 'sdkconfig.defaults'};{overlay}"
    command = [
        args.idf,
        "-B",
        str(build_dir),
        f"-DSDKCONFIG={sdkconfig}",
        f"-DSDKCONFIG_DEFAULTS={defaults}",
        "build",
    ]
    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)
    return build_dir


def build_and_flash(args, port: str) -> None:
    if args.target == "panda" and not args.allow_panda_flash:
        raise HilError("Panda flashing requires --allow-panda-flash")
    overlay, build_dir = profile_paths(args)
    sdkconfig = build_dir / "sdkconfig.profile"
    defaults = f"{ROOT / 'sdkconfig.defaults'};{overlay}"
    command = [
        args.idf,
        "-B",
        str(build_dir),
        f"-DSDKCONFIG={sdkconfig}",
        f"-DSDKCONFIG_DEFAULTS={defaults}",
        "-p",
        port,
        "build",
        "flash",
    ]
    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def remote_flash_command(
    args, build_dir: pathlib.Path, remote_firmware_dir: str
) -> tuple[list[str], list[pathlib.Path]]:
    manifest_path = build_dir / "flasher_args.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HilError(f"cannot load flash manifest {manifest_path}: {exc}") from exc

    extra = manifest.get("extra_esptool_args", {})
    command = [
        args.remote_esptool,
        "--chip",
        str(extra.get("chip", "esp32c3")),
        "--port",
        args.port,
        "--baud",
        str(args.flash_baud),
        "--before",
        str(extra.get("before", "default_reset")),
        "--after",
        str(extra.get("after", "hard_reset")),
    ]
    if extra.get("stub") is False:
        command.append("--no-stub")
    command.extend(["write_flash", *manifest.get("write_flash_args", [])])

    sources = []
    flash_files = manifest.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise HilError(f"{manifest_path}: flash_files is missing or empty")
    for offset, relative in sorted(
        flash_files.items(), key=lambda item: int(item[0], 0)
    ):
        source = build_dir / relative
        if not source.is_file():
            raise HilError(f"flash image is missing: {source}")
        sources.append(source)
        command.extend(
            [offset, posixpath.join(remote_firmware_dir, source.name)]
        )
    return command, sources


def run_remote(args) -> int:
    if not args.port:
        raise HilError("--host requires an explicit --port on the remote machine")
    args.transport = resolve_transport(args.target, args.transport, args.port)
    if args.target == "panda" and args.flash and not args.allow_panda_flash:
        raise HilError("Panda flashing requires --allow-panda-flash")

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_dir or ROOT / "hil-results" / stamp
    output_dir.mkdir(parents=True, exist_ok=True)

    remote_root = args.remote_dir.rstrip("/")
    if not remote_root:
        raise HilError("--remote-dir must not be empty")
    remote_tools = posixpath.join(remote_root, "tools")
    remote_scenarios = posixpath.join(remote_root, "tests", "hil", "scenarios")
    remote_firmware = posixpath.join(remote_root, "firmware")
    remote_results = posixpath.join(remote_root, "results", stamp)
    mkdir_command = [
        "mkdir",
        "-p",
        remote_tools,
        remote_scenarios,
        remote_firmware,
        remote_results,
    ]
    subprocess.run(
        ["ssh", args.host, shlex.join(mkdir_command)],
        cwd=ROOT,
        check=True,
    )

    subprocess.run(
        [
            "scp",
            str(pathlib.Path(__file__).resolve()),
            f"{args.host}:{remote_tools}/hil.py",
        ],
        cwd=ROOT,
        check=True,
    )
    scenario_paths = sorted(SCENARIO_DIR.glob("*.json"))
    if args.scenario:
        scenario = load_scenario(args.scenario, args.target)
        selected_path = args.scenario.resolve()
        scenario_paths = [
            path for path in scenario_paths if path.name != selected_path.name
        ]
        scenario_paths.append(selected_path)
        if not scenario["steps"]:
            raise HilError(f"{args.scenario}: scenario has no steps")
    subprocess.run(
        [
            "scp",
            *(str(path) for path in scenario_paths),
            f"{args.host}:{remote_scenarios}/",
        ],
        cwd=ROOT,
        check=True,
    )

    if args.flash:
        build_dir = build_profile(args)
        flash_command, sources = remote_flash_command(
            args, build_dir, remote_firmware
        )
        subprocess.run(
            [
                "scp",
                *(str(path) for path in sources),
                f"{args.host}:{remote_firmware}/",
            ],
            cwd=ROOT,
            check=True,
        )
        subprocess.run(
            ["ssh", args.host, shlex.join(flash_command)],
            cwd=ROOT,
            check=True,
        )
        time.sleep(1.0)

    remote_runner = [
        args.remote_python,
        posixpath.join(remote_tools, "hil.py"),
        "--target",
        args.target,
        "--transport",
        args.transport,
        "--port",
        args.port,
        "--baud",
        str(args.baud),
        "--output-dir",
        remote_results,
    ]
    if args.suite:
        remote_runner.append("--suite")
    elif args.interactive:
        remote_runner.append("--interactive")
    else:
        remote_runner.extend(
            [
                "--scenario",
                posixpath.join(remote_scenarios, args.scenario.name),
            ]
        )
    if args.allow_heater:
        remote_runner.append("--allow-heater")
    if args.verbose:
        remote_runner.append("--verbose")

    result = subprocess.run(
        ["ssh", args.host, shlex.join(remote_runner)],
        cwd=ROOT,
        check=False,
    )
    expected_results = (
        ("serial.jsonl",) if args.interactive else ("report.json", "serial.jsonl")
    )
    missing_results = []
    for name in expected_results:
        copied = subprocess.run(
            [
                "scp",
                f"{args.host}:{posixpath.join(remote_results, name)}",
                str(output_dir / name),
            ],
            cwd=ROOT,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if copied.returncode != 0:
            missing_results.append(name)
    if missing_results:
        joined = ", ".join(missing_results)
        raise HilError(f"remote run did not return expected results: {joined}")

    report_path = output_dir / "report.json"
    if report_path.is_file():
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["host"] = args.host
        report["transcript"] = str(output_dir / "serial.jsonl")
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"local results: {output_dir}", flush=True)
    return result.returncode


def run_scenario(
    transport: SerialTransport,
    scenario: dict[str, Any],
    target: str,
    allow_heater: bool,
) -> dict[str, Any]:
    variables: dict[str, Any] = {}
    steps_report: list[dict[str, Any]] = []
    name = scenario.get("name", "unnamed")
    print(f"\n{name}")

    for index, step in enumerate(scenario["steps"], start=1):
        step_name = step.get("name", f"step {index}")
        started = time.monotonic()
        report: dict[str, Any] = {"name": step_name, "passed": False}
        try:
            if "wait_s" in step:
                time.sleep(float(step["wait_s"]))
                response: Optional[dict[str, Any]] = None
            else:
                command = substitute(step.get("send"), variables)
                if not isinstance(command, dict):
                    raise HilError("step requires send object or wait_s")
                if target == "panda" and requests_heat(command) and not allow_heater:
                    raise HilError(
                        "positive-heat command refused on Panda; pass --allow-heater"
                    )
                response = transport.request(
                    command, timeout_s=float(step.get("timeout_s", 4.0))
                )
                check_expectations(response, step.get("expect", {"ok": True}))
                for variable, path in step.get("save", {}).items():
                    variables[variable] = dotted_get(response, path)
                report["response"] = response
            report["passed"] = True
            print(f"  PASS  {step_name}")
        except Exception as exc:
            report["error"] = str(exc)
            print(f"  FAIL  {step_name}: {exc}")
            steps_report.append(report)
            return {"name": name, "passed": False, "steps": steps_report}
        finally:
            report["duration_s"] = round(time.monotonic() - started, 3)
        steps_report.append(report)
    return {"name": name, "passed": True, "steps": steps_report}


def interactive(transport: SerialTransport, target: str, allow_heater: bool) -> int:
    print("Enter one JSON command per line; Ctrl-D exits.")
    for line in sys.stdin:
        try:
            command = json.loads(line)
            if not isinstance(command, dict):
                raise HilError("command must be a JSON object")
            if target == "panda" and requests_heat(command) and not allow_heater:
                raise HilError("positive heat refused; restart with --allow-heater")
            print(json.dumps(transport.request(command), indent=2, sort_keys=True))
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
    return 0


def parse_args(argv: Optional[list[str]] = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device; auto-detected if exactly one")
    parser.add_argument("--baud", type=int, default=115200, help="HIL console baud")
    parser.add_argument("--target", choices=("devboard", "panda"), default="devboard")
    parser.add_argument(
        "--transport",
        choices=("native", "uart"),
        help="console transport; inferred from ttyACM/ttyUSB when omitted",
    )
    parser.add_argument("--scenario", type=pathlib.Path, help="run one scenario JSON file")
    parser.add_argument("--suite", action="store_true", help="run all scenarios for target")
    parser.add_argument("--interactive", action="store_true", help="read JSON commands from stdin")
    parser.add_argument("--flash", action="store_true", help="build and flash before testing")
    parser.add_argument("--build-dir", help="override the isolated profile build directory")
    parser.add_argument("--idf", default="idf.py", help="ESP-IDF idf.py executable")
    parser.add_argument("--host", help="run flash and HIL I/O on an SSH host")
    parser.add_argument(
        "--remote-python", default="python3", help="remote Python with pyserial"
    )
    parser.add_argument(
        "--remote-esptool", default="esptool.py", help="remote esptool executable"
    )
    parser.add_argument(
        "--remote-dir", default="/tmp/dragonbreath-hil", help="remote staging root"
    )
    parser.add_argument(
        "--flash-baud", type=int, default=460800, help="esptool transfer baud"
    )
    parser.add_argument(
        "--allow-heater",
        action="store_true",
        help="allow positive-target commands on a real Panda",
    )
    parser.add_argument(
        "--allow-panda-flash",
        action="store_true",
        help="allow replacing firmware on a real Panda",
    )
    parser.add_argument("--output-dir", type=pathlib.Path, help="local result directory")
    parser.add_argument("--verbose", action="store_true", help="print protocol traffic")
    args = parser.parse_args(argv)
    selected = sum((bool(args.scenario), args.suite, args.interactive))
    if selected != 1:
        parser.error("select exactly one of --scenario, --suite, or --interactive")
    return args


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    try:
        if args.host:
            return run_remote(args)
        port = resolve_port(args.port)
        args.transport = resolve_transport(args.target, args.transport, port)
        if args.flash:
            build_and_flash(args, port)
            time.sleep(1.0)

        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        output_dir = args.output_dir or ROOT / "hil-results" / stamp
        output_dir.mkdir(parents=True, exist_ok=True)
        transcript_path = output_dir / "serial.jsonl"
        report_path = output_dir / "report.json"

        with transcript_path.open("w", encoding="utf-8") as transcript:
            transport = SerialTransport(port, args.baud, transcript, args.verbose)
            try:
                transport.drain(1.0)
                if args.interactive:
                    result = interactive(transport, args.target, args.allow_heater)
                    print(f"serial: {transcript_path}")
                    return result

                scenarios = (
                    load_suite_scenarios(args.target)
                    if args.suite
                    else [(args.scenario, load_scenario(args.scenario, args.target))]
                )
                results = []
                for _, scenario in scenarios:
                    results.append(
                        run_scenario(
                            transport, scenario, args.target, args.allow_heater
                        )
                    )
                    if not results[-1]["passed"]:
                        break
            finally:
                transport.close()

        report = {
            "target": args.target,
            "transport": args.transport,
            "port": port,
            "passed": bool(results) and all(item["passed"] for item in results),
            "scenarios": results,
            "transcript": str(transcript_path),
        }
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"\n{'PASS' if report['passed'] else 'FAIL'}")
        print(f"report: {report_path}")
        print(f"serial: {transcript_path}")
        return 0 if report["passed"] else 1
    except (HilError, OSError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
