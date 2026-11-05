#!/bin/sh
# ally.sh run wrapper for G-Diffuser on the Ally.
#
# logind reaps the whole SSH session scope on disconnect (KillUserProcesses), which killed
# the game seconds after launch even through ally.sh's setsid/nohup detach. Handing the game
# to the user systemd manager as a transient service keeps it alive after the session ends.
#
# Usage: ally-run-gdiffuser.sh <log-file> [game args...]   (cwd must be the run directory)
# Side effect: ally's pid tracking only sees this wrapper (it exits immediately), so
# `ally stop` cannot kill the game -- use: systemctl --user stop gdiffuser-run

xdg=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export XDG_RUNTIME_DIR=$xdg
export DBUS_SESSION_BUS_ADDRESS=unix:path=$xdg/bus

log=$1
shift

systemctl --user stop gdiffuser-run.service >/dev/null 2>&1

# Pass GDX_TRACE/GDX_LOG through only when set: systemd-run starts a clean environment,
# so an exported trace gate in the caller's shell would otherwise never reach the game.
# The determinism experiment's script, seed and probe variables travel the same way.
trace_env=""
for name in GDX_TRACE GDX_LOG GDX_INPUT_SCRIPT GDX_RAND_SEED1 GDX_RAND_SEED2 GDX_DET_LOG GDX_DET_DUMP GDX_DET_RACE_SEED; do
    eval "value=\${$name}"
    [ -n "$value" ] && trace_env="$trace_env $name=$value"
done

exec systemd-run --user --collect --same-dir --unit=gdiffuser-run \
    --property=StandardOutput=append:"$log" \
    --property=StandardError=append:"$log" \
    env WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0} XDG_RUNTIME_DIR=$xdg $trace_env ./G-Diffuser "$@"
