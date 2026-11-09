#!/usr/bin/env python3
"""Install, remove, diagnose or collect stock data for Wireless Android Auto."""

import argparse
import datetime
import getpass
import ipaddress
import os

from installer.build import version
from installer.device import Device, connection_stage
from installer.diagnostics import collect
from installer.interface import (
    ACTIONS,
    action_header,
    choose_action,
    connection_header,
    menu_actions,
    show_error,
    show_interrupted,
    show_result,
    show_saved,
    show_status,
)
from installer.paths import ROOT
from installer.progress import reporting, step, summary
from installer.stock import collect as collect_stock
from installer.workflow import inspect, install, remove

LAST_HOST = ROOT / "local" / "last-host"


def remembered_host():
    try:
        return str(ipaddress.IPv4Address(LAST_HOST.read_text().strip()))
    except (OSError, ValueError):
        return None


def suggested_host():
    """The last MIB while still on its network, else the gateway setup.sh found:
    on the MIB access point the gateway is the MIB itself."""
    last = remembered_host()
    try:
        gateway = ipaddress.IPv4Address(os.environ.get("WAA_GATEWAY", ""))
    except ValueError:
        return last
    if last and ipaddress.IPv4Address(last) in ipaddress.IPv4Network(
        f"{gateway}/24", strict=False
    ):
        return last
    return str(gateway)


def ask_host():
    default = suggested_host()
    prompt = f" MIB address [{default}]: " if default else " MIB address: "
    while True:
        value = input(prompt).strip() or (default or "")
        try:
            return str(ipaddress.IPv4Address(value))
        except ValueError:
            print(" Enter an IPv4 address, for example 192.0.2.1.")


def remember_host(host):
    try:
        LAST_HOST.write_text(host + "\n")
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser(
        prog="./setup.sh",
        description="Wireless Android Auto: install, remove, diagnose and collect stock data",
    )
    parser.add_argument(
        "action",
        nargs="?",
        choices=tuple(action for action, _, _ in ACTIONS),
        help="Skip the menu and start this action",
    )
    args = parser.parse_args()
    os.umask(0o077)
    connection_header()
    host = ask_host()
    password = getpass.getpass(" Root password [root]: ") or "root"
    print()
    with reporting():
        step("Connect to MIB")
        try:
            device = Device(host, password)
        finally:
            del password
        remember_host(host)
        try:
            with connection_stage("MIB: inspect installed state"):
                state = inspect(device)
        except BaseException:
            device.close()
            raise
        summary(host if device.active else host + " · passive FTP, slow downloads")
    try:
        print()
        show_status(host, state, version())
        actions = menu_actions(state)
        if args.action:
            # Same entry as the menu would show, e.g. "Update" for install.
            choice = next(
                (a for a in (*actions, *ACTIONS) if a[0] == args.action), None
            )
        else:
            choice = choose_action(actions)
        if choice is None:
            return 0
        action, label, _ = choice
        action_header(label)
        print()
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        # Captures and compiler outputs must live in the persistent private volume.
        prefix = {"diagnose": "debug-", "collect-stock": "stock-"}.get(action, "")
        directory = ROOT / "local" / (prefix + stamp)
        os.environ["WAA_WORKDIR"] = str(directory)
        try:
            with reporting():
                operation = {
                    "install": install,
                    "remove": remove,
                    "diagnose": collect,
                    "collect-stock": collect_stock,
                }[action]
                result = operation(device, directory)
            show_result(result, state.get("wifi"))
            return 1 if result.status == "unsupported_firmware" else 0
        finally:
            if directory.exists():
                logs = ["build.log"] if (directory / "build.log").exists() else []
                show_saved(directory, ROOT, logs)
    finally:
        device.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, OSError) as error:
        show_error(error)
        raise SystemExit(1)
    except (KeyboardInterrupt, EOFError):
        show_interrupted()
        raise SystemExit(130)
