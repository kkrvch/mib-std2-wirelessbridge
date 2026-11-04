"""Full private capture, with content and attribute checks before publication."""

import os
from pathlib import Path, PurePosixPath

from installer.elf import is_sha256, require, sha256
from installer.firmware import INPUTS, UnsupportedFirmware, resolve
from installer.paths import REMOTE, ROOT, json_bytes
from installer.progress import detail, item, item_done, size, summary

# Large originals of recognised firmware, addressed by SHA-256.
CACHED = ("wa", "sal", "libc", "gal")


def cache_dir():
    return Path(os.environ.get("WAA_CACHE", ROOT / "local/cache"))


def cached(device, remote):
    """A cached copy of `remote`, or None. Hashing on the unit needs a size match first,
    so the checksum helper never runs for files that cannot be in the cache."""
    directory = cache_dir()
    if not directory.is_dir():
        return None
    expected = device.size(remote)
    candidates = [
        p
        for p in directory.iterdir()
        if is_sha256(p.name)
        and p.is_file()
        and not p.is_symlink()
        and p.stat().st_size == expected
    ]
    if not candidates:
        return None
    digest = device.sha256(remote)
    for path in candidates:
        if path.name == digest:
            data = path.read_bytes()
            if sha256(data) == digest:
                return data
    return None


def remember(directory, records):
    cache = cache_dir()
    cache.mkdir(parents=True, mode=0o700, exist_ok=True)
    for name in CACHED:
        target = cache / records[name]["sha256"]
        if target.is_file():
            continue
        temporary = cache / (".new-" + target.name)
        temporary.write_bytes((directory / "inputs" / INPUTS[name]).read_bytes())
        temporary.chmod(0o600)
        os.replace(temporary, target)


def download(device, directory, *, reuse=True):
    # Update's checksum helper may already have created the workspace.
    directory.mkdir(parents=True, mode=0o700, exist_ok=True)
    require(not (directory / "inputs").exists(), "Capture workspace already has inputs")
    records = {}
    total = reused = 0
    for index, (name, remote) in enumerate(REMOTE.items(), 1):
        item(PurePosixPath(remote).name, index, len(REMOTE))
        attrs = device.attrs(remote)
        data = cached(device, remote) if reuse and name in CACHED else None
        if data is not None:
            reused += 1
            detail("Reusing verified cached original: " + name)
        if data is None:
            data = device.read(remote)
        require(device.attrs(remote) == attrs, "File changed during capture: " + name)
        relative = INPUTS.get(name, "config/" + name)
        path = directory / "inputs" / relative
        path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o600)
        records[name] = {"sha256": sha256(data), "attrs": attrs}
        total += len(data)
        detail(f"Saved {size(len(data))}, attributes verified")
        item_done()
    return records, total, reused


def capture(device, directory):
    records, total, reused = download(device, directory)
    detail("Identifying firmware and checking ELF compatibility")
    try:
        binding = resolve(directory / "inputs")
    except UnsupportedFirmware:
        save_unprofiled(device, directory, records)
        raise
    record = {"schema": 1, "profile": binding["profile"], "files": records}
    (directory / "snapshot.json").write_bytes(json_bytes(record))
    unchanged(device, records)
    remember(directory, records)
    detail(f"Profile: {binding['profile']}. All originals rechecked.")
    summary(
        f"{len(REMOTE)} files · {size(total)}"
        + (f" · {reused} reused" if reused else "")
        + f" · {binding['profile']}"
    )
    return record


def save_unprofiled(device, directory, records):
    """Recheck by a second FTP read: no profile is needed, and no generated
    checksum helper runs on firmware that may be unknown."""
    unchanged(device, records, reread=True)
    (directory / "snapshot.json").write_bytes(
        json_bytes({"schema": 1, "profile": None, "files": records})
    )


def unchanged(device, records, *, reread=False):
    for name, record in records.items():
        detail("Checking original " + name + " on MIB (SHA-256)")
        digest = (
            sha256(device.read(REMOTE[name])) if reread else device.sha256(REMOTE[name])
        )
        require(
            device.attrs(REMOTE[name]) == record["attrs"]
            and digest == record["sha256"],
            "Device input changed: " + name,
        )
