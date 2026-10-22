"""Install the SDL3 proxy next to Hytale, keeping the original library recoverable."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

OWNER = "aurora-shaders-sdl-proxy-v1"
MARKER = b"AURORA_SDL_PROXY_V1"
DLSS_MARKER = b"AURORA_LINUX_DLSS_V1"
MAC = sys.platform == "darwin"
# On macOS the libraries live inside the signed app bundle; the pack stays outside, in Client/.aurora.
MACOS = "Hytale.app/Contents/MacOS/"
EXECUTABLE, LIBRARY, BACKUP = ((MACOS + "HytaleClient", MACOS + "libSDL3.dylib", MACOS + "libSDL3.aurora-original.dylib")
                               if MAC else ("HytaleClient", "libSDL3.so", "libSDL3.aurora-original.so"))
SIGNED = (EXECUTABLE, "Hytale.app/Contents/_CodeSignature/CodeResources")   # what re-signing rewrites


def digest(path):
    with path.open("rb") as stream:
        result = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def is_proxy(path):
    return path.is_file() and MARKER in path.read_bytes()


def is_native(header):
    """The game's own binary format: arm64 Mach-O on macOS, x86_64 ELF on Linux."""
    if MAC:
        return header[:8] == b"\xcf\xfa\xed\xfe\x0c\x00\x00\x01"
    return len(header) >= 20 and header[:6] == b"\x7fELF\x02\x01" and header[18:20] == b"\x3e\x00"


def resign(client):
    """Ad-hoc signature without the hardened runtime, whose library validation would reject the proxy."""
    subprocess.run(["codesign", "--force", "--sign", "-", "--preserve-metadata=identifier,entitlements",
                    str(client / "Hytale.app")], check=True)


def save_signature(client, directory):
    """Keep Hypixel's signed executable and seal, so uninstall restores them byte for byte."""
    if subprocess.run(["codesign", "--verify", "--strict", str(client / "Hytale.app")]).returncode:
        raise ValueError("Hytale's code signature is not intact; repair the game in the launcher first")
    (directory / "signature").mkdir(exist_ok=True)
    for name in SIGNED:
        atomic_copy(client / name, directory / "signature" / Path(name).name)


def restore_signature(client, directory):
    for name in SIGNED:
        atomic_copy(directory / "signature" / Path(name).name, client / name)


def require_stopped(client):
    if MAC:
        if subprocess.run(["pgrep", "-x", "HytaleClient"], capture_output=True).returncode == 0:
            raise ValueError("Close Hytale before installing or removing the automatic hook")
        return
    executable = str(client / "HytaleClient")
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            running = os.readlink(entry / "exe").removesuffix(" (deleted)")
        except OSError:
            continue
        if running == executable:
            raise ValueError("Close Hytale before installing or removing the automatic hook")


def state(client):
    directory = client / ".aurora"
    if directory.is_symlink():
        raise ValueError(f"Refusing to write through the existing symlink: {directory}")
    manifest = directory / "installation.json"
    if not manifest.exists():
        if directory.exists() and any(directory.iterdir()):
            raise ValueError(f"Unrecognized Aurora folder; move it aside first: {directory}")
        return directory, None
    saved = json.loads(manifest.read_text())
    if saved.get("owner") != OWNER:
        raise ValueError(f"Unrecognized installation record: {manifest}")
    return directory, saved


def atomic_copy(source, target):
    fd, name = tempfile.mkstemp(prefix=".aurora-", dir=target.parent)
    os.close(fd)
    pending = Path(name)
    try:
        shutil.copy2(source, pending)
        pending.replace(target)
    finally:
        pending.unlink(missing_ok=True)


def merge_settings(defaults, previous, dlss_supported=False):
    scalar = r"(?m)^(#define\s+(AU_\w+)\s+)([-+]?\d+(?:\.\d+)?)(?=\s|$)"
    values = {match[1]: match[2] for match in re.findall(scalar, previous)}
    # Older Linux installers zeroed every DLSS option, including motion/jitter
    # multipliers. Detect that signature without overwriting individual user edits.
    legacy_motion = ("AU_DLSS_JITTER", "AU_DLSS_JITTER_SX", "AU_DLSS_JITTER_SY",
                     "AU_DLSS_MOTION_SX", "AU_DLSS_MOTION_SY")
    legacy_dlss_zeroed = all(name in values and float(values[name]) == 0 for name in legacy_motion)

    def update(match):
        name = match[2]
        value = values.get(name, match[3])
        if legacy_dlss_zeroed and name.startswith("AU_DLSS_") and float(value) == 0:
            value = match[3]
        # Disable the backend, not its tuning values: they must survive a later
        # driver repair or an upgrade to a DLSS-capable installation.
        if not dlss_supported and name in ("AU_DLSS", "AU_RENDER_SCALE"):
            value = "0"
        if name == "AU_DLSS_FRAMEGEN":
            value = "0"
        if not dlss_supported and name == "AU_GI_DENOISER":
            value = "1"
        return match[1] + value

    return re.sub(scalar, update, defaults)


def write_state(directory, saved):
    fd, name = tempfile.mkstemp(prefix=".installation-", dir=directory)
    with os.fdopen(fd, "w") as stream:
        json.dump(saved, stream, indent=2)
        stream.write("\n")
    pending = Path(name)
    try:
        pending.replace(directory / "installation.json")
    finally:
        pending.unlink(missing_ok=True)


def install(client, proxy, pack):
    require_stopped(client)
    directory, saved = state(client)
    library, backup = client / LIBRARY, client / BACKUP
    if MAC and saved and is_proxy(library) and digest(client / EXECUTABLE) != saved.get("signed_sha256"):
        # An update replaced the executable under the hook: undo first so its new signature is saved.
        uninstall(client)
        directory, saved = state(client)
    if library.is_symlink() or backup.is_symlink():
        raise ValueError("SDL3 or its backup is a symlink; refusing to replace it")
    if not is_proxy(proxy):
        raise ValueError("Build the automatic hook first with ./build.sh")
    if not library.is_file():
        raise ValueError(f"The game's SDL3 library is missing: {library}")
    hooked = is_proxy(library)
    if hooked and (not saved or digest(library) != saved.get("proxy_sha256")):
        raise ValueError("The installed hook has no matching recovery record; refusing to overwrite it")
    if backup.exists() and (not saved or digest(backup) != saved.get("original_sha256")):
        raise ValueError(f"The existing SDL3 backup is unrecognized or changed: {backup}")
    if hooked and not backup.is_file():
        raise ValueError("Original SDL3 backup is missing; repair Hytale before reinstalling Aurora")
    original = backup if hooked else library
    with original.open("rb") as stream:
        header = stream.read(20)
    if not is_native(header):
        raise ValueError("Expected Hytale's native SDL3 library")
    record = {"owner": OWNER, "original_sha256": digest(original),
              "proxy_sha256": digest(proxy), "installed": True}
    directory.mkdir(exist_ok=True)
    installed_pack = directory / "shaderpack"
    if installed_pack.is_symlink():
        raise ValueError(f"Refusing to replace the existing symlink: {installed_pack}")
    # Stage every file before changing the library that Hytale loads.
    with tempfile.TemporaryDirectory(prefix=".install-", dir=directory) as temporary:
        stage = Path(temporary)
        shutil.copytree(pack, stage / "shaderpack", ignore=shutil.ignore_patterns("hook.log", "errors", "dump"))
        settings = installed_pack / "include/AU_Settings.glsl"
        staged_settings = stage / "shaderpack/include/AU_Settings.glsl"
        previous_presets = installed_pack / "presets"
        previous_sss = installed_pack / "sss"
        if saved and previous_sss.is_dir() and not previous_sss.is_symlink():
            shutil.copytree(previous_sss, stage / "shaderpack/sss", dirs_exist_ok=True)
            assets = client.parent / "Assets.zip"
            if assets.is_file():
                from build_sss import build_masks
                (stage / "shaderpack/sss-masks.bin").write_bytes(build_masks(assets, stage / "shaderpack/sss"))
            elif (installed_pack / "sss-masks.bin").is_file():
                shutil.copy2(installed_pack / "sss-masks.bin", stage / "shaderpack/sss-masks.bin")
        if saved and previous_presets.is_dir() and not previous_presets.is_symlink():
            shutil.copytree(previous_presets, stage / "shaderpack/presets", dirs_exist_ok=True)
        if staged_settings.is_file():
            previous = settings.read_text(encoding="utf-8-sig") if saved and settings.is_file() else ""
            staged_settings.write_text(merge_settings(staged_settings.read_text(encoding="utf-8-sig"), previous, DLSS_MARKER in proxy.read_bytes()), encoding="utf-8")
        shutil.copy2(original, stage / "original.so")
        shutil.copy2(proxy, stage / "proxy.so")
        old_pack = stage / "previous-pack"
        if installed_pack.exists():
            installed_pack.replace(old_pack)
        signed = False
        try:
            (stage / "shaderpack").replace(installed_pack)
            if MAC and not hooked:
                backup.unlink(missing_ok=True)   # our copy, left when an update replaced the proxy, is outside the new seal
                save_signature(client, directory)
            write_state(directory, record)
            if not hooked:
                atomic_copy(stage / "original.so", backup)
            atomic_copy(stage / "proxy.so", library)
            if MAC:
                signed = True
                resign(client)
                record["signed_sha256"] = digest(client / EXECUTABLE)
                write_state(directory, record)
        except Exception:
            # A failed install must leave a usable vanilla game and recoverable settings.
            atomic_copy(stage / "original.so", library)
            if signed:
                restore_signature(client, directory)
            record["installed"] = False
            write_state(directory, record)
            if old_pack.exists():
                shutil.rmtree(installed_pack)
                old_pack.replace(installed_pack)
            raise
    return installed_pack


def uninstall(client):
    require_stopped(client)
    directory, saved = state(client)
    library, backup = client / LIBRARY, client / BACKUP
    if not saved:
        if is_proxy(library):
            raise ValueError("Cannot restore SDL3 without an Aurora recovery record")
        return
    if library.is_symlink() or backup.is_symlink():
        raise ValueError("SDL3 or its backup is a symlink; refusing to replace it")
    if is_proxy(library):
        if digest(library) != saved.get("proxy_sha256"):
            raise ValueError("The installed hook was changed; refusing to overwrite it")
        if not backup.is_file() or digest(backup) != saved.get("original_sha256"):
            raise ValueError("Original SDL3 backup is missing or changed; repair Hytale to restore it")
        atomic_copy(backup, library)
        # Hypixel's signature only goes back onto the executable it was taken from, never an updated one.
        if MAC and digest(client / EXECUTABLE) == saved.get("signed_sha256"):
            restore_signature(client, directory)
    # If an update already replaced the proxy, retain the newer game library.
    if backup.is_file() and digest(backup) == saved.get("original_sha256"):
        backup.unlink()
    if MAC:
        shutil.rmtree(directory / "signature", ignore_errors=True)
    saved["installed"] = False
    write_state(directory, saved)


def installed(client):
    _, saved = state(client)
    return bool(saved and saved.get("installed") and is_proxy(client / LIBRARY))
