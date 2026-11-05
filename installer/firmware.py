"""Select a verified WA/SAL pair; resolve public ELF interfaces automatically.

Profiles contain internal implementation knowledge, never copied OEM instructions.
The resolved binding and generated headers belong to the private build directory.
"""

import json
from pathlib import Path

from installer.elf import Image, Refuse, arm_bl_decode, require, sha256
from installer.paths import ROOT, workdir

INPUTS = {
    "wa": "firmware/wirelessadapter/tsd.mibstd2.system.wirelessadapter.main",
    "sal": "firmware/sal/tsd.mibstd2.sal.main",
    "libc": "runtime/libc.so.3",
    "gal": "runtime/gal.so",
}
SOCKET_EXPORTS = {
    "A_" + name.upper(): name
    for name in (
        "socket",
        "bind",
        "listen",
        "accept",
        "recv",
        "send",
        "shutdown",
        "setsockopt",
        "close",
        "fcntl",
    )
}
EXPORTS = {
    "wa": {"WA_WAIT": "_ZN3tsd6common3ipc4waitEj"},
    "sal": {
        **SOCKET_EXPORTS,
        "A_HANDLETIMEOUT": "_ZN3tsd12connectivity6common3itc6Worker13handleTimeoutEv",
        "SAL_STRING_CTOR": "_ZNSsC1EPKcRKSaIcE",
    },
}
LIBC_EXPORTS = (
    "read",
    "clock_gettime",
    "pthread_self",
    "__get_errno_ptr",
    "mkfifo",
    "dlopen",
    "dlsym",
    "dlclose",
    "fsync",
)


def build_dir(output=None):
    return Path(output or workdir() / "build")


def profiles():
    return [
        json.loads(p.read_text()) for p in sorted((ROOT / "profiles").glob("*.json"))
    ]


class UnsupportedFirmware(Refuse):
    """No verified profile covers these firmware inputs; collect for porting."""


def select(wa, sal, candidates=None):
    hashes = {"wa": sha256(wa), "sal": sha256(sal)}
    matches = [
        p
        for p in (profiles() if candidates is None else candidates)
        if all(p[role]["sha256"] == h for role, h in hashes.items())
    ]
    if not matches:
        raise UnsupportedFirmware("Unknown WA/SAL combination: " + str(hashes))
    require(len(matches) == 1, "Ambiguous WA/SAL combination: " + str(hashes))
    require(matches[0]["schema"] == 1, "Unsupported firmware profile schema")
    return matches[0]


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def resolve_role(raw, spec, role):
    require(sha256(raw) == spec["sha256"], f"Changed {role} input")
    image = Image(raw, "ET_EXEC")
    runtime_exports = spec.get("runtime_exports", [])
    require(
        isinstance(runtime_exports, list)
        and all(isinstance(name, str) for name in runtime_exports)
        and len(runtime_exports) == len(set(runtime_exports))
        and (not runtime_exports or role == "sal")
        and set(runtime_exports) <= SOCKET_EXPORTS.keys(),
        "Invalid runtime exports",
    )
    names = [*spec["functions"], *spec.get("data", {}), *spec["layout"], *EXPORTS[role]]
    require(
        len(names) == len(set(names)) and not any(n.startswith("QR_") for n in names),
        "Profile overrides another binding or a runtime ELF symbol",
    )
    constants = {name: number(v) for name, v in spec["functions"].items()}
    for name, address in constants.items():
        require(address % 4 == 0, f"Unaligned internal function {name}")
        image.offset(address, 4, flags=1)
    for name, value in spec.get("data", {}).items():
        address = number(value)
        image.mapped(address, 4)
        constants[name] = address
    for macro, name in EXPORTS[role].items():
        require(macro not in constants, "Profile overrides public ELF symbol: " + macro)
        if macro in runtime_exports:
            require(
                not any(s.name == name for s in image.symbols()),
                "Runtime export already present in ELF: " + name,
            )
        else:
            constants[macro] = image.function(name)
    constants.update({name: number(v) for name, v in spec["layout"].items()})
    constants["QR_READ_GOT"] = image.got("read")
    constants.update(
        {
            "QR_" + name.upper() + "_PLT": image.function(name)
            for name in ("read", "write", "open", "close")
        }
    )
    hooks = []
    occupied = set()
    for hook in spec["hooks"]:
        address = number(hook["at"])
        require(
            address % 4 == 0 and address not in occupied, "Overlapping/unaligned hooks"
        )
        occupied.add(address)
        require(hook["kind"] in ("bl", "word", "keep"), "Unknown hook kind")
        image.offset(address, 4, flags=1 if hook["kind"] == "bl" else 0)
        image.unrelocated(address)
        expected = constants[hook["target"]]
        actual = image.word(address)
        if hook["kind"] == "bl":
            actual = arm_bl_decode(actual, address)
        require(actual == expected, f"Unexpected {role} hook at {address:#x}")
        hooks.append(dict(hook, at=address, expected=expected))
    image.note_header()
    result = {
        "sha256": sha256(raw),
        "base": image.base,
        "constants": constants,
        "hooks": hooks,
    }
    if runtime_exports:
        result["runtime_exports"] = {
            macro: SOCKET_EXPORTS[macro] for macro in runtime_exports
        }
    return result


def reader_layout(profile):
    """Validate the address-only contract for the existing ARM GAL guard."""
    spec = profile["gal_reader"]
    offsets = []
    for start, size in spec["shape_ranges"]:
        start, size = number(start), number(size)
        require(
            start >= 0
            and start % 4 == 0
            and 0 < size <= 256
            and size % 4 == 0
            and start + size <= 4096,
            "Invalid GAL reader signature range",
        )
        offsets.extend(range(start, start + size))
    require(
        0 < len(offsets) <= 256 and len(offsets) == len(set(offsets)),
        "Empty, oversized or overlapping GAL reader signature",
    )
    result = {
        key: number(spec[key]) for key in ("copy_call", "read_call", "plt_max_distance")
    }
    for key in ("copy_call", "read_call"):
        require(
            0 <= result[key] < 4096 and result[key] % 4 == 0, "Invalid GAL call offset"
        )
    require(result["copy_call"] != result["read_call"], "Overlapping GAL calls")
    require(0 < result["plt_max_distance"] <= 0x1000000, "Invalid GAL PLT distance")
    return dict(result, shape_offsets=offsets)


def resolve(inputs=None):
    inputs = Path(inputs or workdir() / "inputs")
    raw = {role: (inputs / path).read_bytes() for role, path in INPUTS.items()}
    profile = select(raw["wa"], raw["sal"])
    # Symbol lookup determines addresses, not ABI compatibility.
    if sha256(raw["libc"]) not in profile["libc_sha256"]:
        raise UnsupportedFirmware("Unverified QNX libc ABI")
    result = {
        "schema": 1,
        "profile": profile["id"],
        "profile_sha256": sha256(json.dumps(profile, sort_keys=True).encode()),
    }
    for role in ("wa", "sal"):
        result[role] = resolve_role(raw[role], profile[role], role)
    if sha256(raw["gal"]) not in profile["gal_sha256"]:
        raise UnsupportedFirmware("Unverified GAL implementation")
    gal = Image(raw["gal"], "ET_DYN")
    reader = gal.function("_ZN15GalReaderThread3runEv")
    layout = reader_layout(profile)
    shape = bytes(
        gal.read(reader + offset, 1, flags=1)[0] for offset in layout["shape_offsets"]
    )
    require(
        sha256(shape) == profile["gal_reader_shape_sha256"], "Unexpected GAL reader"
    )
    copy_call = reader + layout["copy_call"]
    copy_plt = arm_bl_decode(gal.word(copy_call), copy_call)
    require(
        reader - layout["plt_max_distance"] <= copy_plt <= reader,
        "GAL copy PLT is outside the guard's supported range",
    )
    gal.plt("memcpy", copy_plt)
    read_call = reader + layout["read_call"]
    gal.offset(arm_bl_decode(gal.word(read_call), read_call), 4, flags=1)
    result["gal"] = {
        **layout,
        "sha256": sha256(raw["gal"]),
        "reader": reader,
        "shape": shape.hex(),
        "memcpy_got": gal.got("memcpy"),
    }
    libc = Image(raw["libc"], "ET_DYN")
    result["libc"] = {"sha256": sha256(raw["libc"]), "functions": {}}
    for name in LIBC_EXPORTS:
        address = libc.function(name)
        result["libc"]["functions"][name] = {
            "address": address,
            "signature": libc.read(address, 16, flags=1).hex(),
        }
    return result


def write_headers(binding, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    profile = [
        "/* Generated compatibility metadata. */",
        "#ifndef WAA_PROFILE_GENERATED_H",
        "#define WAA_PROFILE_GENERATED_H",
        "#define WAA_STRING_INNER(x) #x",
        "#define WAA_STRING(x) WAA_STRING_INNER(x)",
    ]
    runtime = ["/* Private: signatures derived from the user's libc. */"]
    runtime += [
        f"#define GAL_READER 0x{binding['gal']['reader']:x}u",
        f"#define GAL_MEMCPY_GOT 0x{binding['gal']['memcpy_got']:x}u",
        f"#define GAL_COPY_CALL 0x{binding['gal']['copy_call']:x}u",
        f"#define GAL_READ_CALL 0x{binding['gal']['read_call']:x}u",
        f"#define GAL_PLT_MAX_DISTANCE 0x{binding['gal']['plt_max_distance']:x}u",
        "static const unsigned short gal_shape_offsets[]={"
        + ",".join(str(x) for x in binding["gal"]["shape_offsets"])
        + "};",
        "static const unsigned char gal_shape[]={"
        + ",".join(str(x) for x in bytes.fromhex(binding["gal"]["shape"]))
        + "};",
    ]
    for role in ("wa", "sal"):
        profile.append("#if defined(WAA_ROLE_" + role.upper() + ")")
        for name, value in sorted(binding[role]["constants"].items()):
            profile.append(f"#define {name} 0x{value:x}")
        if exports := binding[role].get("runtime_exports"):
            profile.extend(f"static unsigned {name};" for name in exports)
            profile.append(
                "#define WAA_RUNTIME_EXPORTS(X) "
                + " ".join(f'X({macro}, "{name}")' for macro, name in exports.items())
            )
        profile.append("#endif")
    profile.append("#endif")
    for name, spec in binding["libc"]["functions"].items():
        runtime.append(f"#define QR_{name.upper()} 0x{spec['address']:x}u")
        runtime.append(
            f"static const unsigned char qr_sig_{name}[16]={{"
            + ",".join(str(x) for x in bytes.fromhex(spec["signature"]))
            + "};"
        )
    (output / "profile_generated.h").write_text("\n".join(profile) + "\n")
    (output / "runtime_generated.h").write_text("\n".join(runtime) + "\n")
    (output / "binding.json").write_text(json.dumps(binding, indent=2) + "\n")


def checked_binding(output=None):
    """Reject inputs/profile changed since compilation, including stale private outputs."""
    output = build_dir(output)
    current = resolve()
    require(
        json.loads((output / "binding.json").read_text()) == current,
        "Inputs or profile changed since build; rebuild first",
    )
    return current


BUILD_FILES = (
    "wa_target.elf",
    "sal_target.elf",
    "restore.elf",
    "wirelessadapter.restore",
)


def source_digest():
    paths = sorted(
        [
            *(ROOT / "native").rglob("*.c"),
            *(ROOT / "native").rglob("*.h"),
            *(ROOT / "native").rglob("*.py"),
            *(ROOT / "native").rglob("*.sh"),
            *(ROOT / "installer").glob("*.py"),
        ]
    )
    return sha256(
        json.dumps(
            {str(p.relative_to(ROOT)): sha256(p.read_bytes()) for p in paths},
            sort_keys=True,
        ).encode()
    )


def build_record(binding, output):
    return {
        "binding_sha256": sha256(json.dumps(binding, sort_keys=True).encode()),
        "source_sha256": source_digest(),
        "files": {name: sha256((output / name).read_bytes()) for name in BUILD_FILES},
    }


def finish_build(output=None):
    output = build_dir(output)
    record = build_record(checked_binding(output), output)
    (output / "build-manifest.json").write_text(json.dumps(record, indent=2) + "\n")


def checked_build(output=None):
    output = build_dir(output)
    binding = checked_binding(output)
    require(
        (output / "build-manifest.json").is_file(), "Incomplete build; rebuild first"
    )
    require(
        json.loads((output / "build-manifest.json").read_text())
        == build_record(binding, output),
        "Build artifacts or sources changed; rebuild first",
    )
    return binding
