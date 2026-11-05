# Development

Back to the [README](../README.md).

## Repository layout

The repository separates computer-side setup from code deployed to the MIB:

- `setup.sh` — the user-facing launcher; prepares Docker and starts setup.
- `installer/` — Python code running on the computer in Docker: the menu,
  installation/removal, diagnostics, firmware patching and verification.
  `installer/__main__.py` is its internal entry point (`python -m installer`).
- `native/` — code deployed to the MIB: C runtime extensions with their build/link
  tools, and the boot selection and service launch scripts in `native/boot/`.
- `profiles/` — supported firmware metadata and hook definitions.
- `local/` — private captures, backups and generated outputs; excluded from publication.

## Offline builds

Offline builds require your own compatible device inputs under
`local/inputs/` (firmware, runtime libraries and configuration). Build and verify
a package with:

```sh
docker build --target runtime -t mib-std2-wirelessbridge-waa .
docker run --rm -v "$PWD/local:/app/local" -v "$PWD/installer:/app/installer:ro" \
    -v "$PWD/native:/app/native:ro" -v "$PWD/profiles:/app/profiles:ro" \
    --entrypoint python mib-std2-wirelessbridge-waa \
    -m installer.build --out /app/local/package
```

The image contains only the toolchain; the code is mounted read-only at run time.
`setup.sh` labels the image with a checksum of the `Dockerfile` and skips the build
while it matches, which keeps setup usable offline.

Repeated integrity checks compute SHA-256 on MIB with a temporary verified command
that is removed when setup disconnects. Originals cached in `local/cache/` are reused
only after their size and hash match the unit's copy.

## Adding a firmware version

A firmware is supported only through a profile in `profiles/`: the SHA-256 of its
WirelessAdapter and SAL binaries, the addresses of the internal functions, data and
structure offsets the runtime uses, the hook sites, and the accepted libc and GAL
versions. Setup refuses any WA/SAL pair without an exact profile match.

Older SAL builds may omit socket imports used by the bridge. A profile can list
these in `sal.runtime_exports`; only known socket APIs absent from that executable
are allowed. The bridge resolves them from loaded libraries before initialization
and leaves the stock Worker running if any function is unavailable.

To request a new version, run **Collect stock data** on that unit and open an issue
with the firmware version. Any installation attempt on an unknown version creates the
same archive automatically.
