# To be sourced by scripts to setup initial environment

ENV_SCR=$(readlink -f "${BASH_SOURCE}")
TOOLS_DIR=$(dirname "${ENV_SCR}")
LOCAL_ROOT=$(dirname "${TOOLS_DIR}")
if [ ! -z "${SNIPER_ROOT:-}" ]; then
  export GRAPHITE_ROOT="${SNIPER_ROOT}"
elif [ ! -z "${GRAPHITE_ROOT:-}" ]; then
  export SNIPER_ROOT="${GRAPHITE_ROOT}"
else
  export SNIPER_ROOT="${LOCAL_ROOT}"
  export GRAPHITE_ROOT="${SNIPER_ROOT}"
fi
if [ ! -f "${SNIPER_ROOT}/run-sniper" ]; then
  echo "Error: SNIPER_ROOT appears to be invalid: [${SNIPER_ROOT}]"
  exit 1
fi
if [ "${LOCAL_ROOT}" != "${SNIPER_ROOT}" ]; then
  echo "Warning: Running from a Sniper directory that is different from your SNIPER_ROOT [${LOCAL_ROOT}] != [${SNIPER_ROOT}]"
fi

# Find the most common benchmarks directory locations, and automatically setup the environment if it exists
if [ -z "${BENCHMARKS_ROOT:-}" ]; then
  if [ -f "${SNIPER_ROOT}/../benchmarks/run-sniper" ] ; then
    export BENCHMARKS_ROOT=$(readlink -f "${SNIPER_ROOT}/../benchmarks")
  elif [ -f "${SNIPER_ROOT}/benchmarks/run-sniper" ] ; then
    export BENCHMARKS_ROOT=$(readlink -f "${SNIPER_ROOT}/benchmarks")
  elif [ -f "${SNIPER_ROOT}/../run-sniper" ] ; then
    export BENCHMARKS_ROOT=$(readlink -f "${SNIPER_ROOT}/..")
  fi
fi
