#!/usr/bin/env bash
# Build a Bela project onto the board and run it.
#
#   ./scripts/deploy.sh <source-dir> <project-name> [extra build_project.sh args]
#
# Environment:
#   BELA_SCRIPTS   where Bela's own scripts live on this machine (default ~/Bela/scripts)
#   BBB_HOSTNAME   the board's address (default 192.168.7.2; also bela.local or a LAN IP)
#
# UNTESTED — needs a Bela on the network. Phase 0.

set -euo pipefail

SRC="${1:?usage: deploy.sh <source-dir> <project-name> [args...]}"
NAME="${2:?usage: deploy.sh <source-dir> <project-name> [args...]}"
shift 2

BELA_SCRIPTS="${BELA_SCRIPTS:-$HOME/Bela/scripts}"
export BBB_HOSTNAME="${BBB_HOSTNAME:-192.168.7.2}"

if [ ! -x "$BELA_SCRIPTS/build_project.sh" ]; then
	echo "build_project.sh not found in $BELA_SCRIPTS" >&2
	echo "clone https://github.com/BelaPlatform/Bela and set BELA_SCRIPTS" >&2
	exit 1
fi

echo "deploying $SRC as '$NAME' to $BBB_HOSTNAME"
"$BELA_SCRIPTS/build_project.sh" "$SRC" -p "$NAME" "$@"
