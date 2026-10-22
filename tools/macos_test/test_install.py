from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import linux_hook

PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.example.hytale-test</string>
<key>CFBundleExecutable</key><string>HytaleClient</string>
</dict></plist>
"""


def compile_c(output, source, *flags):
    subprocess.run(["cc", "-arch", "arm64", *flags, "-x", "c", "-", "-o", str(output)],
                   input=source.encode(), check=True)


def make_app(client, version, library=None):
    """A signed stand-in for Hytale.app: hardened runtime, like Hypixel's build."""
    macos = client / "Hytale.app/Contents/MacOS"
    macos.mkdir(parents=True)
    (macos.parent / "Info.plist").write_text(PLIST)
    compile_c(macos / "HytaleClient", f"int main(void) {{ return {version}; }}")
    if library:
        (macos / "libSDL3.dylib").write_bytes(library)
    else:
        compile_c(macos / "libSDL3.dylib", f"int SDL_GetVersion(void) {{ return {version}; }}", "-dynamiclib")
    subprocess.run(["codesign", "--force", "--sign", "-", "--options", "runtime", str(client / "Hytale.app")],
                   check=True, capture_output=True)


def snapshot(app):
    return {path.relative_to(app): path.read_bytes() for path in sorted(app.rglob("*")) if path.is_file()}


def signature(app):
    details = subprocess.run(["codesign", "-dv", str(app)], capture_output=True, text=True).stderr
    valid = subprocess.run(["codesign", "--verify", "--strict", str(app)], capture_output=True).returncode == 0
    return valid, "runtime" in details


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("cc"), "needs macOS with the Xcode command line tools")
class MacInstallTests(unittest.TestCase):
    @patch.object(linux_hook, "require_stopped")
    def test_install_resigns_and_uninstall_restores_the_bundle(self, stopped):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client, app = root / "Client", root / "Client/Hytale.app"
            make_app(client, 1)
            vanilla = snapshot(app)
            proxy = root / "proxy.dylib"
            compile_c(proxy, '__attribute__((used)) static const char marker[] = "AURORA_SDL_PROXY_V1";', "-dynamiclib")
            pack = root / "pack"
            (pack / "include").mkdir(parents=True)
            (pack / "include/AU_Settings.glsl").write_text("#define AU_DLSS 0\n")

            linux_hook.install(client, proxy, pack)
            self.assertTrue(linux_hook.installed(client))
            self.assertEqual((client / linux_hook.BACKUP).read_bytes(), vanilla[Path("Contents/MacOS/libSDL3.dylib")])
            self.assertEqual(signature(app), (True, False))   # valid ad-hoc seal, no hardened runtime
            linux_hook.install(client, proxy, pack)            # a reinstall keeps the first signature backup
            linux_hook.uninstall(client)
            self.assertEqual(snapshot(app), vanilla)
            self.assertEqual(signature(app), (True, True))

            linux_hook.install(client, proxy, pack)
            make_app(root / "update", 2, vanilla[Path("Contents/MacOS/libSDL3.dylib")])
            for name in linux_hook.SIGNED:                     # an update that leaves SDL (our proxy) alone
                shutil.copy2(root / "update" / name, client / name)
            patched = snapshot(root / "update/Hytale.app")
            linux_hook.install(client, proxy, pack)
            self.assertEqual(signature(app), (True, False))
            linux_hook.uninstall(client)
            self.assertEqual(snapshot(app), patched)            # never the older executable
            self.assertEqual(signature(app), (True, True))

            linux_hook.install(client, proxy, pack)
            shutil.rmtree(app)                                 # a game update replaces the whole bundle
            make_app(client, 3)
            updated = snapshot(app)
            linux_hook.uninstall(client)
            self.assertEqual(snapshot(app), updated)
            self.assertFalse((client / ".aurora/signature").exists())


if __name__ == "__main__":
    unittest.main()
