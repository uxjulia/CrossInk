#!/bin/sh
# Terminal command and Finder launcher. The Python entry point chooses the source checkout.
tooling=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
exec python3 "$tooling/scripts/x4pro_local/launch.py" "$@"
