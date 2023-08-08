#!/usr/bin/env bash

set -euo

src_dir="$(cd "$(dirname "$0")" && pwd)/../.."
example_dir="$src_dir/examples"

if [[ "$OSTYPE" == "msys" ]]; then
  # On Windows, convert path from /c/Users to C:/Users so python understands it.
  example_dir=$(cygpath -m "$example_dir")
fi

if [[ ! -f "./mapget" && ! -f "./mapget.exe" ]]; then
  echo "Please launch this script from the mapget build/bin directory."
  exit 1
fi

# This will store the PID of mapget
mapget_pid=0

trap '
  echo "Done!"
  if [[ $mapget_pid -ne 0 ]]; then
    # Kill mapget using its PID
    kill $mapget_pid || true
  fi
' EXIT

port_to_launch_on=$1

./mapget --log-level trace serve \
  --port $port_to_launch_on \
  --datasource-exe "python $example_dir/python/datasource.py" &

# Capture the PID of mapget
mapget_pid=$!

# Wait for the mapget process to finish
wait $mapget_pid
