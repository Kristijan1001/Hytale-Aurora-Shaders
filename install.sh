#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -s) == Darwin ]]; then
    # Homebrew's Python is externally managed, so Pillow (block textures) goes into a private venv.
    venv="$root/build/venv"
    "$venv/bin/python3" -c 'import PIL' 2>/dev/null \
        || { python3 -m venv "$venv" && "$venv/bin/pip" install --quiet --disable-pip-version-check pillow; }
    exec "$venv/bin/python3" "$root/tools/linux_install.py" "$@"
fi
exec python3 "$root/tools/linux_install.py" "$@"
