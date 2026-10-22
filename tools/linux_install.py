#!/usr/bin/env python3
"""Install Aurora's automatic SDL3 hook for the native Linux Hytale client."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess

from prepare_shaders import ROOT, prepare
import linux_hook
import linux_dlss

APP_ID = "com.hypixel.HytaleLauncher"
CONFIG = ROOT / "build" / "linux-install.json"
CLIENT_SUFFIX = Path("install/release/package/game/latest/Client")


def candidates(home=None, data_home=None):
    home = home or Path.home()
    if linux_hook.MAC:
        return [(home / "Library/Application Support/Hytale" / CLIENT_SUFFIX, "native")]
    data_home = data_home or Path(os.environ.get("XDG_DATA_HOME", home / ".local/share"))
    return [
        (data_home / "Hytale" / CLIENT_SUFFIX, "native"),
        (home / ".var/app" / APP_ID / "data/Hytale" / CLIENT_SUFFIX, "flatpak"),
    ]


def is_client(path):
    return (path / linux_hook.EXECUTABLE).is_file()


def detect(client_dir=None, backend=None):
    if client_dir:
        client = client_dir.expanduser().resolve()
        if not is_client(client):
            raise ValueError(f"No native HytaleClient found in {client}")
        mode = "flatpak" if APP_ID in client.parts else "native"
        return client, backend or mode
    if CONFIG.is_file():
        saved = json.loads(CONFIG.read_text())
        client = Path(saved["client_dir"])
        if (not backend or saved["backend"] == backend) and is_client(client):
            return client, backend or saved["backend"]
    for client, mode in candidates():
        if (not backend or mode == backend) and is_client(client):
            return client, mode
    raise ValueError("Hytale was not found. Run ./install.sh --client-dir /path/to/Client")


def validate_client(executable):
    with executable.open("rb") as stream:
        header = stream.read(20)
    if not linux_hook.is_native(header):
        raise ValueError("Expected the native HytaleClient for this system, not a Windows/Wine client")


def fingerprint(executable):
    stat = executable.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def launch_command(config, extra, env):
    """Optional shortcut to the normal launcher; the installed proxy handles loading."""
    if config["backend"] == "flatpak":
        return ["flatpak", "run", APP_ID, *extra], env.copy()
    launcher = config.get("launcher")
    if not launcher or not Path(launcher).is_file():
        raise ValueError("Open your normal Hytale launcher, or configure --launcher /path/to/hytale-launcher")
    return [launcher, *extra], env.copy()


def build_proxy(force=False):
    proxy = ROOT / ("build/libaurora-sdl.dylib" if linux_hook.MAC else "build/libaurora-sdl.so")
    dlss_supported = not linux_hook.MAC and linux_dlss.configure()
    build_env = os.environ.copy()
    build_env["AU_DLSS"] = "1" if dlss_supported else "0"
    previous_dlss = proxy.is_file() and linux_hook.DLSS_MARKER in proxy.read_bytes()
    inputs = [ROOT / "hook/aud_dlss.c", ROOT / "build.sh", *(ROOT / "hook").glob("*.h"), ROOT / "hook/hook.c", ROOT / "hook/sdl_proxy.map"]
    if force or previous_dlss != dlss_supported or not proxy.is_file() or any(p.stat().st_mtime_ns > proxy.stat().st_mtime_ns for p in inputs):
        subprocess.run(["bash", str(ROOT / "build.sh")], check=True, env=build_env)
    return proxy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-dir", type=Path, help="Folder containing the native HytaleClient")
    parser.add_argument("--backend", choices=("native", "flatpak"), help="Override launcher detection")
    parser.add_argument("--launcher", type=Path, help="Native hytale-launcher executable")
    parser.add_argument("--build", action="store_true", help="Rebuild even if a library is already present")
    parser.add_argument("--uninstall", action="store_true", help="Restore the original SDL3 library and keep shader settings")
    parser.add_argument("--launch", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--print-command", action="store_true", help="Show launch command without starting the game")
    args, extra = parser.parse_known_args()
    if extra and extra[0] == "--":
        extra = extra[1:]
    if extra and not args.launch:
        parser.error("Unrecognized arguments: " + " ".join(extra))
    try:
        if (platform.system(), platform.machine()) not in (("Linux", "x86_64"), ("Darwin", "arm64")):
            raise ValueError("Aurora targets Ubuntu-based x86_64 Linux and Apple Silicon macOS")
        if args.uninstall:
            client, _ = detect(args.client_dir, args.backend)
            linux_hook.uninstall(client)
            CONFIG.unlink(missing_ok=True)
            print("Aurora's automatic hook removed. Start Hytale normally for vanilla shaders.\n"
                  f"Your installed shader settings have been kept in {client / '.aurora/shaderpack'}")
            return
        if args.launch:
            if not CONFIG.is_file():
                raise ValueError("Run ./install.sh before launching Aurora")
            config = json.loads(CONFIG.read_text())
            executable = Path(config["client_dir"]) / linux_hook.EXECUTABLE
            if not executable.is_file() or fingerprint(executable) != config["client_fingerprint"]:
                raise ValueError("Hytale has changed since shader preparation. Close the launcher and run ./install.sh again")
            if not linux_hook.installed(Path(config["client_dir"])):
                raise ValueError("Aurora's automatic hook is missing. Run ./install.sh again")
            argv, env = launch_command(config, extra, os.environ)
            if args.print_command:
                import shlex
                print(shlex.join(argv))
                return
            print("Starting Hytale with Aurora. Your menu hotkey (Home by default) opens options; Pause toggles shaders.", flush=True)
            os.execvpe(argv[0], argv, env)
        client, backend = detect(args.client_dir, args.backend)
        executable = client / linux_hook.EXECUTABLE
        validate_client(executable)
        launcher = args.launcher.expanduser().resolve() if args.launcher else None
        linux_hook.require_stopped(client)
        if backend == "native":
            if not launcher:
                launcher = next((p / "hytale-launcher" for p in client.parents if (p / "hytale-launcher").is_file()), None)
        proxy = build_proxy(args.build)
        prepare(executable, ROOT / "shaderpack", client.parent / "Assets.zip")
        installed_pack = linux_hook.install(client, proxy, ROOT / "shaderpack")
        config = {"client_dir": str(client), "backend": backend,
                  "client_fingerprint": fingerprint(executable), "launcher": str(launcher) if launcher else None,
                  "automatic": True, "installed_pack": str(installed_pack)}
        CONFIG.parent.mkdir(exist_ok=True)
        pending = CONFIG.with_suffix(".tmp")
        pending.write_text(json.dumps(config, indent=2) + "\n")
        pending.replace(CONFIG)
        hotkey = "Home by default, fn+Left on a Mac keyboard" if linux_hook.MAC else "Home by default"
        print(f"Aurora's automatic hook installed for {backend}: {client}\n"
              f"Start Hytale normally. Your menu hotkey ({hotkey}) opens options; rebind it under General."
              f"{'' if linux_hook.MAC else ' launch.sh is optional.'}\n"
              f"Live shader settings: {installed_pack / 'include/AU_Settings.glsl'}\n"
              "Run ./install.sh again after game updates. Use ./install.sh --uninstall to restore vanilla.")
    except (OSError, ValueError, KeyError, ImportError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Aurora: {error}\n")


if __name__ == "__main__":
    main()
