"""Private build inside Docker. The user-facing setup exposes install/remove/diagnostics."""

import os
import re
import secrets
import signal
import subprocess
import sys
from contextlib import redirect_stdout
from pathlib import Path

from installer import configuration
from installer.elf import require, sha256
from installer.firmware import INPUTS, checked_build
from installer.patching import patch_image, write_new
from installer.paths import (
    PATCHED,
    RECOVERY,
    ROOT,
    VERSION_ID,
    json_bytes,
    workdir,
)
from installer.progress import detail, enabled

FILES = (
    *PATCHED.values(),
    RECOVERY,
    "select.sh",
    "run.sh",
    "cpu.conf",
)


def new_release():
    return secrets.token_hex(8)


def version():
    """Setup checkout version passed in by setup.sh; "unknown" without git."""
    value = os.environ.get("WAA_VERSION", "")
    return value if re.fullmatch(VERSION_ID, value) else "unknown"


def check_shell(path):
    """Preserve shell errors and byte-level evidence from the failed package."""
    result = subprocess.run(
        ["sh", "-n", str(path)],
        capture_output=True,
        text=True,
        errors="backslashreplace",
    )
    if not result.returncode:
        return
    data = path.read_bytes()
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    bare_cr = data.count(b"\r") - crlf
    nul = data.count(b"\x00")
    bom = data.startswith(b"\xef\xbb\xbf")
    metadata = (
        f"bytes={len(data)}, CRLF={crlf}, LF={lf}, "
        f"bare CR={bare_cr}, UTF-8 BOM={bom}, NUL={nul}"
    )
    report = [
        f"Shell syntax check failed: {path}",
        f"Command: sh -n {path}",
        f"Exit status: {result.returncode}",
        f"Setup version: {version()}",
        f"Shell: {Path('/bin/sh').resolve()}",
        f"SHA256: {sha256(data)}",
        metadata,
        f"stdout:\n{result.stdout}",
        f"stderr:\n{result.stderr}",
    ]
    # Only bundled boot scripts: a captured stock startup may contain private data.
    if path.name in ("select.sh", "run.sh"):
        report.append("Script bytes by line (escaped; at most 120 lines):")
        lines = data.split(b"\n")
        report.extend(f"{i}: {line!r}" for i, line in enumerate(lines[:120], 1))
        if len(lines) > 120:
            report.append("[remaining lines omitted]")
    log_path = workdir() / "build.log"
    # Packaging may already have redirected buffered Python output to this log.
    sys.stdout.flush()
    with log_path.open("a") as log:
        log.write("\n" + "\n".join(report) + "\n")
    message = result.stderr.strip() or result.stdout.strip() or "No shell output"
    raise RuntimeError(
        f"Shell syntax check failed: {path.name} (exit {result.returncode})\n"
        f"{message}\n{metadata}\nSee log: {log_path}"
    )


def package(output, release):
    output = Path(output)
    require(not output.exists(), "Package directory already exists")
    binding = checked_build()
    build = workdir() / "build"
    inputs = workdir() / "inputs"
    files = {}
    for role, name in PATCHED.items():
        files[name] = patch_image(
            (inputs / INPUTS[role]).read_bytes(),
            (build / (role + "_target.elf")).read_bytes(),
            binding[role],
            enabled=role == "wa",
        )
    files[RECOVERY] = (build / RECOVERY).read_bytes()
    for name in ("select.sh", "run.sh"):
        files[name] = (ROOT / "native/boot" / name).read_bytes()
    files["cpu.conf"] = configuration.cpu(
        (inputs / "config/cpu.conf").read_bytes(), release
    )
    files["startup"] = configuration.startup((inputs / "config/startup").read_bytes())
    for name in ("pf.conf", "pf_wlan.conf"):
        files[name] = configuration.firewall((inputs / "config" / name).read_bytes())
    output.mkdir(parents=True, mode=0o700)
    for name, data in files.items():
        write_new(output / name, data)
        if name.endswith(".sh") or name == "startup":
            check_shell(output / name)
    manifest = {
        "schema": 3,
        "release": release,
        "version": version(),
        "firmware_profile": binding["profile"],
        "profile_sha256": binding["profile_sha256"],
        "files": {name: sha256(data) for name, data in files.items()},
    }
    (output / "manifest.json").write_bytes(json_bytes(manifest))
    from native.runtime.verify_package_code import verify

    verify(output)
    return manifest


def last_line(path):
    """Newest build.log line for the live footer only; the log keeps everything."""
    with path.open("rb") as log:
        log.seek(max(0, log.seek(0, os.SEEK_END) - 4096))
        lines = log.read().decode(errors="replace").splitlines()
    for line in reversed(lines):
        line = "".join(c for c in line if c.isprintable()).strip()
        if line:
            return line
    return None


def build_package(output, release, *, compile=True):
    """Compile once, then package; repackaging for another release skips compilation."""
    os.umask(0o077)
    if not compile:
        return package(output, release)
    command = [sys.executable, str(ROOT / "native/runtime/build.py")]
    env = dict(os.environ, PYTHON=sys.executable)
    if enabled():
        log_path = workdir() / "build.log"
        detail("Compiling WA, SAL and recovery")
        with log_path.open("w") as log:
            with subprocess.Popen(
                command,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            ) as process:
                try:
                    while True:
                        try:
                            result = process.wait(timeout=1)
                            break
                        except subprocess.TimeoutExpired:
                            if line := last_line(log_path):
                                detail(line, plain=False)
                    if result:
                        raise RuntimeError(f"Build failed. See log: {log_path}")
                except BaseException:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    raise
            detail("Compilation complete. Verifying patches and assembling the package")
            with redirect_stdout(log):
                manifest = package(output, release)
        detail("Package built and verified")
    else:
        subprocess.run(command, env=env, check=True)
        manifest = package(output, release)
        print("Private package built:", output)
    return manifest


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Developer-only offline build from private inputs"
    )
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--release", default=new_release())
    args = parser.parse_args()
    build_package(args.out, args.release)
