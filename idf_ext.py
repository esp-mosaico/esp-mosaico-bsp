# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""ESP-IDF extension for flashing Mosaico through its application USB CDC port."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Iterable

import serial
from serial.tools import list_ports


ESPRESSIF_VID = "303a"
APP_PID = "1001"
ROM_PID = "0020"
APPIMAGE_ENV_VARS = (
    "APPDIR",
    "APPIMAGE",
    "LD_LIBRARY_PATH",
    "ELECTRON_RUN_AS_NODE",
    "CMAKE_ROOT",
)


class MosaicoFlashError(RuntimeError):
    """Expected USB flashing failure with a user-facing message."""


def clean_subprocess_env() -> dict[str, str]:
    """Keep editor/AppImage variables from contaminating IDF subprocesses."""
    env = os.environ.copy()
    for name in APPIMAGE_ENV_VARS:
        env.pop(name, None)
    return env


def read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="ascii").strip()
    except (FileNotFoundError, OSError):
        return None


UsbIdentity = tuple[str, str, str, str]


def host_is_linux() -> bool:
    return sys.platform.startswith("linux")


def preferred_serial_device(port: Path) -> Path:
    """Use the macOS callout device; /dev/tty.usbmodem* waits for carrier detect."""
    name = port.name
    if name.startswith("tty.usbmodem") or name.startswith("tty.usbserial"):
        return port.with_name("cu" + name[3:])
    return port


def normalized_usb_serial(serial_number: str) -> str:
    return "".join(character for character in serial_number if character in "0123456789abcdefABCDEF").lower()


def is_mosaico_application_serial(serial_number: str) -> bool:
    """PID 1001 is shared; Mosaico application CDC uses the 12-digit base MAC."""
    return len(normalized_usb_serial(serial_number)) == 12


def usb_parent_for_port(port: Path) -> Path | None:
    try:
        current = (Path("/sys/class/tty") / port.name / "device").resolve()
    except (FileNotFoundError, OSError):
        return None

    for candidate in (current, *current.parents):
        if (candidate / "idVendor").exists() and (candidate / "idProduct").exists():
            return candidate
    return None


def usb_identity_sysfs(port: Path) -> UsbIdentity | None:
    parent = usb_parent_for_port(port)
    if parent is None:
        return None
    vendor = read_text(parent / "idVendor")
    product = read_text(parent / "idProduct")
    serial_number = read_text(parent / "serial") or "unknown"
    if vendor is None or product is None:
        return None
    return parent.name, vendor.lower(), product.lower(), serial_number


def topology_from_location(location: str | None, serial_number: str) -> str:
    if location:
        return location.split(":")[0]
    hex_serial = normalized_usb_serial(serial_number)
    return f"serial-{hex_serial}" if hex_serial else "unknown"


def identity_from_port_info(info: Any) -> UsbIdentity | None:
    if info.vid is None or info.pid is None:
        return None
    serial_number = info.serial_number or "unknown"
    return (
        topology_from_location(info.location, serial_number),
        f"{int(info.vid):04x}",
        f"{int(info.pid):04x}",
        serial_number,
    )


def port_aliases(port: Path) -> set[str]:
    preferred = preferred_serial_device(port)
    names = {port.name, preferred.name}
    try:
        names.add(port.resolve().name)
        names.add(preferred.resolve().name)
    except OSError:
        pass
    return names


def usb_identity_list_ports(port: Path) -> UsbIdentity | None:
    aliases = port_aliases(port)
    for info in list_ports.comports():
        if Path(info.device).name not in aliases:
            continue
        identity = identity_from_port_info(info)
        if identity is not None:
            return identity
    return None


def usb_identity(port: Path) -> UsbIdentity | None:
    return usb_identity_sysfs(port) or usb_identity_list_ports(port)


def iter_linux_acm_ports() -> Iterable[tuple[Path, UsbIdentity]]:
    for port in sorted(Path("/dev").glob("ttyACM*")):
        identity = usb_identity_sysfs(port)
        if identity is not None:
            yield port, identity


def iter_list_ports() -> Iterable[tuple[Path, UsbIdentity]]:
    for info in list_ports.comports():
        device = Path(info.device)
        if sys.platform == "darwin" and not device.name.startswith("cu."):
            continue
        identity = identity_from_port_info(info)
        if identity is not None:
            yield preferred_serial_device(device), identity


def iter_mosaico_ports() -> Iterable[tuple[Path, UsbIdentity]]:
    if host_is_linux():
        yield from iter_linux_acm_ports()
        return
    yield from iter_list_ports()


def matching_ports(topology: str | None = None, product: str | None = None) -> list[Path]:
    matches: list[Path] = []
    for port, identity in iter_mosaico_ports():
        device_topology, vendor, device_product, serial_number = identity
        if vendor != ESPRESSIF_VID:
            continue
        if topology is not None and device_topology != topology:
            continue
        if product is not None and device_product != product:
            continue
        if product == APP_PID and not is_mosaico_application_serial(serial_number):
            continue
        matches.append(port)
    return matches


def select_port(requested_port: str | None) -> tuple[Path, UsbIdentity]:
    if requested_port is not None:
        port = preferred_serial_device(Path(requested_port)).resolve()
        identity = usb_identity(port)
        if identity is None:
            raise MosaicoFlashError(f"{requested_port} is not an enumerated USB serial port")
        if identity[1] != ESPRESSIF_VID or identity[2] not in (APP_PID, ROM_PID):
            raise MosaicoFlashError(
                f"{requested_port} is {identity[1]}:{identity[2]}, "
                "expected Mosaico application USB 303a:1001 or ROM USB 303a:0020"
            )
        return port, identity

    candidates = matching_ports(product=APP_PID) or matching_ports(product=ROM_PID)
    if len(candidates) != 1:
        rendered = ", ".join(str(port) for port in candidates) or "none"
        raise MosaicoFlashError(
            f"expected one Mosaico USB port, found: {rendered}; pass -p/--port"
        )
    port = candidates[0]
    identity = usb_identity(port)
    assert identity is not None
    return port, identity


def enter_download_mode(port: Path) -> None:
    """Send the USB-Serial/JTAG-compatible DTR/RTS reset sequence."""
    open_kwargs: dict[str, Any] = {"timeout": 0.1}
    if host_is_linux():
        # TIOCEXCL is reliable on Linux ACM. Leave it unset on macOS callout
        # devices, where exclusive open can fail on some CDC stacks.
        open_kwargs["exclusive"] = True
    with serial.Serial(str(port), 115200, **open_kwargs) as device:
        device.rts = False
        device.dtr = False
        time.sleep(0.1)
        device.dtr = True
        device.rts = False
        time.sleep(0.1)
        device.rts = True
        device.dtr = False
        device.rts = True
        time.sleep(0.1)
        device.dtr = False
        device.rts = False


def wait_for_port(topology: str, product: str, timeout_s: float) -> Path:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        ports = matching_ports(topology=topology, product=product)
        if len(ports) == 1:
            return ports[0]
        time.sleep(0.05)
    raise MosaicoFlashError(
        f"timed out waiting for USB device {ESPRESSIF_VID}:{product} "
        f"on topology {topology}"
    )


def load_flasher_config(build_dir: Path) -> dict[str, object]:
    config_path = build_dir / "flasher_args.json"
    try:
        return json.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise MosaicoFlashError(f"missing {config_path}; run idf.py build first") from error
    except json.JSONDecodeError as error:
        raise MosaicoFlashError(f"invalid {config_path}: {error}") from error


def find_idf_py() -> Path:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise MosaicoFlashError("IDF_PATH is not set; source the ESP-IDF export script first")
    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.is_file():
        raise MosaicoFlashError(f"ESP-IDF launcher not found: {idf_py}")
    return idf_py


def flash_rom(port: Path, build_dir: Path, baud: int, config: dict[str, object]) -> None:
    extra = config.get("extra_esptool_args")
    if not isinstance(extra, dict):
        raise MosaicoFlashError("flasher_args.json has no extra_esptool_args object")
    chip = extra.get("chip")
    if not isinstance(chip, str):
        raise MosaicoFlashError("flasher_args.json has no chip name")

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        chip,
        "--port",
        str(port),
        "--baud",
        str(baud),
        "--before",
        "no-reset",
        "--after",
        "hard-reset",
    ]
    if extra.get("stub") is False:
        command.append("--no-stub")
    command.extend(["write-flash", "@flash_args"])
    subprocess.run(command, cwd=build_dir, check=True, env=clean_subprocess_env())


def monitor_application(project_dir: Path, build_dir: Path, port: Path) -> None:
    print(f"Starting IDF Monitor on {port} without another reset...", flush=True)
    subprocess.run(
        [
            sys.executable,
            str(find_idf_py()),
            "-B",
            str(build_dir),
            "-p",
            str(port),
            "monitor",
            "--no-reset",
        ],
        cwd=project_dir,
        check=True,
        env=clean_subprocess_env(),
    )


def run_mosaico_flash(
    requested_port: str | None,
    project_dir: Path,
    build_dir: Path,
    baud: int,
    timeout_s: float,
    start_monitor: bool,
) -> Path:
    if baud <= 0:
        raise MosaicoFlashError("baud must be greater than zero")
    if timeout_s <= 0:
        raise MosaicoFlashError("timeout must be greater than zero")

    config = load_flasher_config(build_dir)
    port, identity = select_port(requested_port)
    topology, _, product, serial_number = identity
    print(f"Using {port} ({topology}, serial {serial_number}, PID {product})", flush=True)

    if product == APP_PID:
        print("Requesting ROM download mode...", flush=True)
        enter_download_mode(port)
        port = wait_for_port(topology, ROM_PID, timeout_s)
        print(f"ROM download port ready: {port}", flush=True)

    flash_rom(port, build_dir, baud, config)
    try:
        app_port = wait_for_port(topology, APP_PID, timeout_s)
    except MosaicoFlashError as error:
        raise MosaicoFlashError(
            f"flash completed, but the application USB port did not return: {error}"
        ) from error
    print(f"Application USB port ready: {app_port}", flush=True)
    if start_monitor:
        monitor_application(project_dir, build_dir, app_port)
    return app_port


def action_extensions(base_actions: dict, project_path: str) -> dict:
    """Register the ``mosaico-flash`` idf.py action."""

    def mosaico_flash(
        subcommand_name: str,
        ctx: Any,
        global_args: Any,
        timeout: float,
        monitor: bool,
    ) -> None:
        del subcommand_name, ctx
        project_dir = Path(global_args.project_dir or project_path).resolve()
        build_dir = Path(global_args.build_dir)
        if not build_dir.is_absolute():
            build_dir = project_dir / build_dir

        try:
            run_mosaico_flash(
                requested_port=global_args.port,
                project_dir=project_dir,
                build_dir=build_dir,
                baud=int(global_args.baud or 460800),
                timeout_s=timeout,
                start_monitor=monitor,
            )
        except (
            MosaicoFlashError,
            OSError,
            serial.SerialException,
            subprocess.CalledProcessError,
        ) as error:
            from idf_py_actions.errors import FatalError

            raise FatalError(f"mosaico-flash: {error}") from error

    return {
        "version": "1",
        "actions": {
            "mosaico-flash": {
                "callback": mosaico_flash,
                "dependencies": ["all"],
                "short_help": "Flash through the Mosaico application USB port",
                "help": (
                    "Build and flash while handling the application-to-ROM USB "
                    "re-enumeration on ESP32-S31 Mosaico."
                ),
                "options": [
                    {
                        "names": ["--timeout"],
                        "type": float,
                        "default": 8.0,
                        "show_default": True,
                        "help": "Seconds to wait for each USB re-enumeration.",
                    },
                    {
                        "names": ["--monitor"],
                        "is_flag": True,
                        "help": "Start IDF Monitor after the application USB port returns.",
                    },
                ],
            }
        },
    }
