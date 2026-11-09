"""Telnet commands and verified FTP transfers; no firmware-specific patching on MIB."""

import ftplib
import gzip
import io
import ipaddress
import json
import re
import shlex
import socket
import time
import uuid
from contextlib import contextmanager
from pathlib import PurePosixPath

from installer.ap_profile import AP_PATH, profile_from_xml
from installer.elf import require, sha256
from installer.paths import (
    BASE,
    CONFIGS,
    RELEASE_ID,
    RELEASES,
    REMOTE,
    ROOT,
    release_dir,
)
from installer.progress import detail, size, transfer
from installer.transport import Session, read_command, wait_prompt

PRIVATE = {"mode": 0o600, "uid": 0, "gid": 0}
EXECUTABLE = dict(PRIVATE, mode=0o700)
# Active-mode FTP data ports; setup.sh publishes the same range from the container.
DATA_PORTS = range(41000, 41032)
# ftpd always connects from port 20 and pf keeps a closed connection's state for a
# while, so a data port is reused only after this long, also across setup runs.
PORT_COOLDOWN = 120  # pf drops closed states after ~90 s
PORT_LOG = ROOT / "local" / "ftp-ports.json"
# A single segment arrives before pf can stall the transfer.
ACTIVE_MINIMUM = 1448


@contextmanager
def connection_stage(label):
    """Identify network failures without logging credentials or server replies."""
    detail(label)
    started = time.monotonic()
    try:
        yield
    except (OSError, EOFError, ftplib.Error) as error:
        reason = (
            "timed out" if isinstance(error, TimeoutError) else type(error).__name__
        )
        raise ConnectionError(
            f"{label}: {reason} ({time.monotonic() - started:.1f}s)"
        ) from error


def quote(path):
    require(
        path.startswith("/") and not any(c in path for c in "\r\n\x00"),
        "Invalid device path",
    )
    return shlex.quote(path)


def data_port():
    """A published data port idle for PORT_COOLDOWN, then marked used; else None."""
    now = time.time()
    try:
        used = {
            int(port): float(at)
            for port, at in json.loads(PORT_LOG.read_text()).items()
        }
    except (OSError, ValueError, TypeError, AttributeError):
        used = {}
    used = {port: at for port, at in used.items() if 0 <= now - at < PORT_COOLDOWN}
    port = next((p for p in DATA_PORTS if p not in used), None)
    if port:
        used[port] = now
        try:
            PORT_LOG.write_text(json.dumps(used))
        except OSError:
            pass
    return port


class Device:
    active = False  # Active-mode FTP downloads, verified at login.

    def __init__(self, host, password, *, connect_ftp=True):
        with connection_stage(f"Telnet: connect to {host}:23"):
            self.session = Session(host, 23)
        self.ftp = None
        self._root_mount_flags = None
        self._checksum_path = None
        try:
            with connection_stage("Telnet: wait for login prompt"):
                wait_prompt(self.session, b"login:")
            with connection_stage("Telnet: send user, wait for password prompt"):
                self.session.send("root")
                wait_prompt(self.session, b"Password:")
            with connection_stage("Telnet: authenticate, wait for root prompt"):
                self.session.send(password)
                wait_prompt(self.session, b"#")
            if connect_ftp:
                with connection_stage(f"FTP: connect to {host}:21 and read greeting"):
                    self.ftp = ftplib.FTP(host, timeout=60)
                with connection_stage("FTP: authenticate as root"):
                    self.ftp.login("root", password)
            # The supported QNX image has pidin but no standalone id utility.
            with connection_stage("Telnet: verify root identity (pidin)"):
                identity = self.command("pidin -p $$ user").decode("ascii")
            rows = re.findall(
                r"(?m)^\s*\d+\s+\S+\s+(\d+)\s+\d+\s+(\d+)\s+\d+\s+(\d+)\s+\d+\s*$",
                identity,
            )
            require(
                rows == [("0", "0", "0")],
                "Cannot verify root shell identity",
            )
            if self.ftp:
                self.active = self._probe_active(host, password)
        except BaseException:
            self.close()
            raise

    def _probe_active(self, host, password):
        """Prefer active FTP downloads: the stock pf drops most data the MIB sends
        on inbound TCP (state from the SYN-ACK misses the peer's window scale),
        so passive downloads crawl at ~3 KB/s. MIB-initiated connections do not."""
        # The client address as the MIB sees it; ftpd checks PORT against it.
        with connection_stage("FTP: STAT (discover client address)"):
            found = re.search(r"Connected to (\S+)", self.ftp.sendcmd("STAT"))
        try:
            self.client = str(ipaddress.IPv4Address(found[1] if found else ""))
        except ValueError:
            detail("FTP: STAT has no client IPv4 address; using passive mode")
            return False
        port = data_port()
        try:
            if port:
                self.ftp.sock.settimeout(10)
                probe = REMOTE["pf.conf"]
                data = bytearray()
                with connection_stage(f"FTP: active download probe on port {port}"):
                    self._retrieve_active(probe, data.extend, port)
                with connection_stage("FTP: SIZE (verify active download)"):
                    expected = self.size(probe)
                if len(data) == expected:
                    self.ftp.sock.settimeout(60)
                    return True
                detail("FTP: active probe size mismatch; using passive mode")
            else:
                detail("FTP: no active probe port available; using passive mode")
        except (OSError, EOFError, ftplib.Error) as error:
            detail(f"FTP: active probe failed ({error}); using passive mode")
        # A failed data connection can leave a reply pending; start over.
        self.ftp.close()
        with connection_stage("FTP: reconnect for passive mode"):
            self.ftp = ftplib.FTP(host, timeout=60)
        with connection_stage("FTP: authenticate again for passive mode"):
            self.ftp.login("root", password)
        return False

    def _retrieve_active(self, path, callback, port):
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("", port))
            listener.listen(1)
            listener.settimeout(10)
            self.ftp.voidcmd("TYPE I")
            self.ftp.voidcmd(
                "PORT "
                + ",".join((*self.client.split("."), str(port >> 8), str(port & 255)))
            )
            # ftpd connects before its preliminary reply; 425 when it cannot.
            reply = self.ftp.sendcmd("RETR " + path)
            if not reply.startswith("1"):
                raise ftplib.error_reply(reply)
            connection, _ = listener.accept()
        with connection:
            connection.settimeout(60)
            while chunk := connection.recv(64 * 1024):
                callback(chunk)
        self.ftp.voidresp()

    def close(self):
        helper = self._checksum_path
        if helper:
            try:
                self.command("rm -f " + quote(helper))
            except (OSError, RuntimeError, ValueError):
                pass
            self._checksum_path = None
        if self.ftp:
            self.ftp.close()
        self.session.close()

    def command(self, command):
        data, rc = read_command(self.session, command)
        require(rc == 0, "Device command failed: " + command)
        return data

    def sha256(self, path):
        from installer.checksum import digest, prepare

        # Advertise the helper only once its upload and self-test succeeded;
        # until then matches() verifies by reading bytes back.
        if not self._checksum_path:
            self._checksum_path = prepare(self)
        return digest(self, self._checksum_path, path)

    def matches(self, path, data):
        return (
            self.sha256(path) == sha256(data)
            if len(data) >= 64 * 1024 or (len(data) >= 4096 and self._checksum_path)
            else self.read(path) == data
        )

    def capture_command(self, command):
        temporary = "/dev/shmem/.waa-debug-" + uuid.uuid4().hex
        q = quote(temporary)
        self.command("(umask 077; set -C; : > " + q + ")")
        try:
            _, status = read_command(self.session, command + " > " + q + " 2>&1")
            return self.read(temporary, compression=6), status
        finally:
            self.command("rm -f " + q)

    def exists(self, path):
        q = quote(path)
        _, rc = read_command(self.session, "test -e " + q + " || test -L " + q)
        require(rc in (0, 1), "Cannot check device path")
        return rc == 0

    def attrs(self, path):
        q = quote(path)
        lines = (
            self.command(
                "test -f " + q + " && test ! -L " + q + " && LC_ALL=C ls -ldn " + q
            )
            .decode("ascii")
            .splitlines()
        )
        rows = [
            re.match(r"^(-[rwx-]{9})\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s", s)
            for s in lines
        ]
        rows = [m for m in rows if m]
        require(
            len(rows) == 1 and rows[0][2] == "1", "Unsupported file attributes: " + path
        )
        m = rows[0]
        bits = sum(1 << (8 - i) for i, c in enumerate(m[1][1:]) if c != "-")
        return {"mode": bits, "uid": int(m[3]), "gid": int(m[4])}

    def _receive(self, path, expected, label=None):
        data = bytearray()
        if expected >= 64 * 1024:
            detail(f"Receiving {label or PurePosixPath(path).name}")

        def received(chunk):
            data.extend(chunk)
            transfer(len(data), expected)

        port = self.active and expected > ACTIVE_MINIMUM and data_port()
        if port:
            self._retrieve_active(path, received, port)
        else:
            self.ftp.retrbinary("RETR " + path, received, blocksize=64 * 1024)
        require(len(data) == expected, "Incomplete file transfer: " + path)
        return bytes(data)

    def _temporary(self):
        path = "/tsd/var/.wireless-aa-transfer-" + uuid.uuid4().hex + ".gz"
        self.command("(umask 077; set -C; : > " + quote(path) + ")")
        return path

    def read(self, path, *, compression=1):
        # SIZE precedes both raw and compressed transfers, including QNX libc,
        # which advertises a size but yields an empty raw FTP download.
        expected = self.size(path)
        if expected < 64 * 1024 and path != REMOTE["libc"]:
            return self._receive(path, expected)
        temporary = self._temporary()
        try:
            require(compression in (1, 6, 9), "Invalid compression level")
            self.command(
                "gzip -"
                + str(compression)
                + " -c "
                + quote(path)
                + " > "
                + quote(temporary)
            )
            packed_size = self.ftp.size(temporary)
            require(
                type(packed_size) is int and packed_size > 0, "Cannot verify gzip size"
            )
            compressed = self._receive(
                temporary, packed_size, PurePosixPath(path).name + " (gzip)"
            )
            try:
                data = gzip.decompress(compressed)
            except (OSError, EOFError) as error:
                raise ValueError("Invalid compressed file transfer: " + path) from error
            require(
                len(data) == expected, "Incomplete compressed file transfer: " + path
            )
            detail(
                f"Verified {PurePosixPath(path).name}: {size(expected)}"
                f" ({size(packed_size)} compressed)"
            )
            return data
        finally:
            self.command("rm -f " + quote(temporary))

    def _send(self, path, data):
        if len(data) < 64 * 1024:
            self.ftp.storbinary("STOR " + path, io.BytesIO(data))
            return
        # Host CPU is cheap; the unit's gunzip speed barely depends on the level.
        compressed = gzip.compress(data, compresslevel=9, mtime=0)
        temporary = self._temporary()
        try:
            detail(
                f"Sending {PurePosixPath(path).name}: {size(len(data))}"
                f" ({size(len(compressed))} compressed)"
            )
            sent = 0

            def uploaded(chunk):
                nonlocal sent
                sent += len(chunk)
                transfer(sent, len(compressed))

            self.ftp.storbinary(
                "STOR " + temporary,
                io.BytesIO(compressed),
                blocksize=64 * 1024,
                callback=uploaded,
            )
            # The caller reads all decompressed bytes back before publication.
            self.command(
                "(umask 077; gzip -d -c " + quote(temporary) + " > " + quote(path) + ")"
            )
        finally:
            self.command("rm -f " + quote(temporary))

    def parents(self, path):
        checks = []
        for parent in reversed(PurePosixPath(path).parents):
            q = quote(str(parent))
            checks.append("test -d " + q + " && test ! -L " + q)
        self.command(" && ".join(checks))

    def mkdir(self, path):
        self.parents(path)
        if not self.exists(path):
            self.command("umask 077 && mkdir " + quote(path) + " && sync")
        self.command("test -d " + quote(path) + " && test ! -L " + quote(path))

    def validate(self, path, kind):
        if kind in ("startup", "shell"):
            self.command("/bin/sh -n " + quote(path))
        elif kind in ("pf.conf", "pf_wlan.conf"):
            self.command("pfctl -n -f " + quote(path))

    def size(self, path):
        quote(path)
        self.ftp.voidcmd("TYPE I")
        value = self.ftp.size(path)
        require(type(value) is int and value >= 0, "Cannot verify file size: " + path)
        return value

    def write(self, path, data, attrs=PRIVATE, kind=None):
        """Upload beside destination, retrieve all bytes, validate, then rename."""
        self._publish(
            path,
            attrs,
            kind,
            lambda pending: self._send(pending, data),
            lambda pending: self.matches(pending, data),
        )

    def copy(self, source, path, attrs=PRIVATE, kind=None, *, sha):
        """Publish an on-device copy whose content must hash to `sha`."""
        self.attrs(source)
        self._publish(
            path,
            attrs,
            kind,
            lambda pending: self.command(
                "(umask 077; cp " + quote(source) + " " + quote(pending) + ")"
            ),
            lambda pending: self.sha256(pending) == sha,
        )

    def _publish(self, path, attrs, kind, fill, verify):
        self.parents(path)
        pending = path + ".waa-new"
        for p in (path, pending):
            if self.exists(p):
                self.attrs(p)  # Reject links, directories, unusual modes and hardlinks.
        fill(pending)
        require(verify(pending), "Uploaded bytes differ: " + path)
        require(
            set(attrs) == {"mode", "uid", "gid"}
            and all(isinstance(v, int) and v >= 0 for v in attrs.values())
            and attrs["mode"] <= 0o777,
            "Invalid file attributes",
        )
        self.command(
            "chown "
            + str(attrs["uid"])
            + ":"
            + str(attrs["gid"])
            + " "
            + quote(pending)
        )
        self.command("chmod " + format(attrs["mode"], "o") + " " + quote(pending))
        self.validate(pending, kind or ("shell" if path.endswith(".sh") else None))
        # Content was verified before the rename; a rename cannot change it.
        self.command("sync && mv " + quote(pending) + " " + quote(path) + " && sync")
        require(self.attrs(path) == attrs, "Published file differs: " + path)

    def unlink(self, path):
        if self.exists(path):
            self.attrs(path)
            self.command("rm " + quote(path) + " && sync")

    def processes(self):
        return self.command("pidin ar")

    def space(self, path, minimum):
        output = self.command("df -kP " + quote(path)).decode("ascii")
        rows = re.findall(r"(?m)^\S+\s+\d+\s+\d+\s+(\d+)\s+\d+%\s+\S+\s*$", output)
        require(
            len(rows) == 1 and int(rows[0]) * 1024 >= minimum,
            "Cannot verify sufficient free space on " + path,
        )

    def preflight(self):
        # Setup can use any reachable interface. Live AP readiness belongs to
        # the runtime, which waits before requesting GAL/HMI preparation.
        profile_from_xml(self.read(AP_PATH))
        processes = self.processes()
        require(
            all(REMOTE[r].encode() in processes for r in ("wa", "sal")),
            "Stock WA/SAL must be running",
        )
        require(
            not any(x in processes for x in (b".waa ", b"wirelessadapter.restore")),
            "Patched service is running",
        )
        for name in ("startup", "pf.conf", "pf_wlan.conf"):
            self.validate(REMOTE[name], name)

    def root_mode(self):
        # QNX's write-open probe can report ENOENT on its read-only namespace.
        # df -g exposes statvfs flags directly; ST_RDONLY is bit zero.
        self.attrs(REMOTE["startup"])
        data = self.command("LC_ALL=C df -g /").decode("ascii")
        flags = re.findall(r"(?m)^\s*Flags\s*:\s*([0-9a-fA-F]+)\s+\[[^\]]*\]\s*$", data)
        require(
            len(flags) == 1
            and len(re.findall(r"(?m)^\S*/hd0t177\s+/\s*$", data)) == 1
            and len(re.findall(r"(?m)^\s*Type\s*:\s*qnx6\s*$", data)) == 1,
            "Cannot determine root mount mode",
        )
        value = int(flags[0], 16)
        if self._root_mount_flags is None:
            self._root_mount_flags = value
        return "ro" if value & 1 else "rw"

    def remount(self, mode, *, restore=False):
        require(mode in ("ro", "rw"), "Invalid root mount mode")
        if self._root_mount_flags is None:
            self.root_mode()
        flags = self._root_mount_flags
        require(flags & ~0x2D == 0, "Unsupported root filesystem flags")
        options = ["remount", mode]
        for bit, option in ((4, "nosuid"), (8, "noatime"), (32, "nocreat")):
            # nocreat must be cleared temporarily to publish startup/PF files.
            if flags & bit and (bit != 32 or mode == "ro" or restore):
                options.append(option)
        mounts = self.command("mount").decode("ascii")
        require(
            len(re.findall(r"(?m)^\S*/hd0t177 on / type qnx6\s*$", mounts)) == 1,
            "Unexpected root filesystem",
        )
        self.command(
            "sync && mount -t qnx6 -o " + ",".join(options) + " /dev/hd0t177 /"
        )
        require(self.root_mode() == mode, "Root remount did not take effect")

    def cleanup(self, keep=()):
        """Delete owned files; with `keep`, only release directories not listed."""
        from installer.build import FILES

        entries = self.entries()
        releases = {
            p[len(RELEASES) + 1 :]
            for p in entries
            if p.startswith(RELEASES + "/")
            and re.fullmatch(RELEASE_ID, p[len(RELEASES) + 1 :])
        }
        names = {
            "install.json",
            "mode",
            "mode.boot",
            "current",
            *("releases/" + r + "/" + n for r in releases for n in FILES),
            *("backup/" + n for n in CONFIGS),
            *(
                "run/" + n
                for n in (
                    "wa2sal.fifo",
                    "sal2wa.fifo",
                    "wa.permit",
                    "sal.permit",
                    "restore-token",
                    "restore-result",
                )
            ),
        }
        files = {BASE + "/" + n for n in names}
        files |= {p + ".waa-new" for p in files}
        dirs = {BASE, BASE + "/backup", BASE + "/run", RELEASES}
        dirs |= {release_dir(r) for r in releases}
        require(
            entries <= files | dirs,
            "Unknown files in installation directory; preserve them before removal",
        )
        if keep:
            for release in sorted(releases - set(keep)):
                prefix = release_dir(release) + "/"
                for path in sorted(p for p in entries & files if p.startswith(prefix)):
                    self.command("rm -f " + quote(path) + " && sync")
                self.command("rmdir " + quote(release_dir(release)) + " && sync")
            return
        # Keep the journal until the final file has gone. A power loss during
        # cleanup can then resume, including from a different computer.
        for path in sorted((entries & files) - {BASE + "/install.json"}):
            self.command("rm -f " + quote(path) + " && sync")
        for path in sorted((entries & dirs) - {BASE}, reverse=True):
            self.command("rmdir " + quote(path) + " && sync")
        self.unlink(BASE + "/install.json")
        self.command("rmdir " + BASE + " && sync")

    def entries(self):
        self.command("test -d " + BASE + " && test ! -L " + BASE)
        links = self.command("find " + BASE + " -type l -print")
        require(
            not any(line.startswith(BASE.encode()) for line in links.splitlines()),
            "Link in installation directory",
        )
        entries = {
            line
            for line in self.command("find " + BASE + " -print")
            .decode("ascii")
            .splitlines()
            if line == BASE or line.startswith(BASE + "/")
        }
        require(BASE in entries, "Incomplete installation directory listing")
        return entries


@contextmanager
def writable_root(device, original):
    require(original in ("ro", "rw"), "Invalid saved root mode")
    nocreat = False
    try:
        mode = device.root_mode()
        flags = getattr(device, "_root_mount_flags", None)
        nocreat = type(flags) is int and bool(flags & 32)
        if mode != "rw" or nocreat:
            device.remount("rw")
        yield
    finally:
        if nocreat:
            # An originally writable filesystem may still prohibit creation.
            # Restore that flag even when the ro/rw mode itself has not changed.
            device.remount(original, restore=True)
        elif device.root_mode() != original:
            device.remount(original)
