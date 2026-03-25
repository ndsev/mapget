#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <ports.env> <command...>" >&2
  exit 2
fi

ports_env="$1"
shift

# shellcheck disable=SC1090
source "$ports_env"

# Note: The wheel test harness passes commands as a single string, so `$MAPGET_SERVER_PORT`
# etc. are not expanded. We intentionally use `eval` here so the variables sourced above
# are expanded before executing the command.
eval "exec $*"

