#!/usr/bin/env bash
# Contract tests against the real core: builds hub-sim, starts it in the
# requested modes (no-auth and/or --auth) on loopback, and runs
# api/tools/contract.py against each, validating every response with
# api/openapi.yaml. Docker only.
#
#   api/contract.sh            both modes
#   api/contract.sh noauth     one mode (noauth | auth)
#
# Each hub-sim runs under a small supervisor so contract.py can restart it
# with the same --data directory (a reboot) and check nothing was lost:
# contract.py writes build/contract-ctl/<mode>/restart (its content: extra
# hub-sim arguments for the next start, e.g. --reset --seed FILE); the
# supervisor stops hub-sim, starts it again and bumps <mode>/started. An
# optional <mode>/prepare ("<record name> <file>") is written into the data
# directory with `hub-sim write-record` while hub-sim is stopped (e.g. a
# config from newer firmware, to test the blocked state). An optional
# <mode>/damage (slot file names, e.g. "secrets.a secrets.b") has
# bytes in the middle of each slot overwritten while hub-sim is stopped, so
# the slot is present but not intact (a torn or rotted write). <mode>/signal
# (USR1 = storage unavailable, USR2 = available) is sent to the running
# hub-sim, which then bumps <mode>/signaled.
set -euo pipefail

cd "$(dirname "$0")/.."
export HOST_UID="$(id -u)" HOST_GID="$(id -g)"
[ $# -eq 0 ] && set -- noauth auth
NOAUTH_PORT=${HUBBO_CONTRACT_PORT:-18080}
AUTH_PORT=$((NOAUTH_PORT + 1))
SIM=hubbo-contract-sim
IMAGE=hubbo-api-lint
CTL=build/contract-ctl

port() { [ "$1" = auth ] && echo "$AUTH_PORT" || echo "$NOAUTH_PORT"; }

docker build -q -t "$IMAGE" -f api/tools/lint.Dockerfile api/tools >/dev/null
# Its own build directory: build/native is shared with other builds running
# at the same time, and a half-rebuilt tree can link inconsistent objects.
docker compose run --rm -T toolchain bash -c '
    [ -f build/contract/build.ninja ] || cmake -S . -B build/contract -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build build/contract --target hub-sim' >/dev/null

rm -rf "$CTL"
supervisors=""
for mode in "$@"; do
    mkdir -p "$CTL/$mode"
    flag=""
    [ "$mode" = auth ] && flag="--auth"
    supervisors+="supervise $mode $(port "$mode") '$flag' & "
done

# Fresh state: each server gets an empty temporary data directory.
docker rm -f "$SIM" >/dev/null 2>&1 || true
trap 'docker rm -f "$SIM" >/dev/null 2>&1 || true' EXIT
docker compose run -d --name "$SIM" -T toolchain bash -c "
    d=\$(mktemp -d)
    supervise() {  # mode port flags
        local ctl=$CTL/\$1 extra='' n=0
        while true; do
            n=\$((n + 1))
            build/contract/sim/hub-sim serve --port \$2 \$3 --data \$d/\$1 \$extra >>\$d/\$1.log 2>&1 &
            local pid=\$!
            echo \$n >\$ctl/started
            local sent=0
            while kill -0 \$pid 2>/dev/null && [ ! -f \$ctl/restart ]; do
                if [ -f \$ctl/signal ]; then
                    kill -\$(cat \$ctl/signal) \$pid
                    rm -f \$ctl/signal
                    sent=\$((sent + 1))
                    echo \$sent >\$ctl/signaled
                fi
                sleep 0.05
            done
            extra=\$(cat \$ctl/restart 2>/dev/null || true)
            kill \$pid 2>/dev/null || true
            wait \$pid 2>/dev/null || true
            if [ -f \$ctl/prepare ]; then
                read -r name file <\$ctl/prepare
                build/contract/sim/hub-sim write-record --data \$d/\$1 --name \$name --file \$file >>\$d/\$1.log 2>&1
                rm -f \$ctl/prepare
            fi
            if [ -f \$ctl/damage ]; then
                for slot in \$(cat \$ctl/damage); do
                    f=\$d/\$1/\$slot
                    [ -f \$f ] || { echo \"damage: no slot \$slot\" >>\$d/\$1.log; continue; }
                    printf XXXXXXXX | dd of=\$f bs=1 seek=\$((\$(stat -c %s \$f) / 2)) conv=notrunc status=none
                done
                rm -f \$ctl/damage
            fi
            rm -f \$ctl/restart
        done
    }
    $supervisors wait" >/dev/null

status=0
for mode in "$@"; do
    docker run --rm --network host --user "$(id -u):$(id -g)" -e HOME=/tmp -e PYTHONDONTWRITEBYTECODE=1 \
        -v "$PWD":/work:ro -v "$PWD/$CTL/$mode":/ctl -w /work "$IMAGE" \
        python -P api/tools/contract.py --base "http://127.0.0.1:$(port "$mode")" --mode "$mode" --ctl /ctl \
        || status=1
done
exit $status
