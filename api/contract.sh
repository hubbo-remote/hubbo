#!/usr/bin/env bash
# Contract tests against the real core: builds hub-sim, starts it in the
# requested modes (no-auth and/or --auth) on loopback, and runs
# api/tools/contract.py against each, validating every response with
# api/openapi.yaml. Docker only.
#
#   api/contract.sh            every mode
#   api/contract.sh noauth     some modes (noauth | auth | mock | mock-auth)
#
# mock and mock-auth run the same flows against the web admin's mock API
# (web/mock, the middleware Vite serves) to catch drift between the mock and
# the hub. It runs standalone (`pnpm mock` in the web service) on
# HUBBO_CONTRACT_PORT+2. What the mock can't support (restarts, storage,
# tokens) is skipped and printed as "SKIPPED (<mode>)".
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
[ $# -eq 0 ] && set -- noauth auth mock mock-auth
NOAUTH_PORT=${HUBBO_CONTRACT_PORT:-18080}
AUTH_PORT=$((NOAUTH_PORT + 1))
MOCK_PORT=$((NOAUTH_PORT + 2))
SIM=hubbo-contract-sim
MOCK=hubbo-contract-mock
IMAGE=hubbo-api-lint
CTL=build/contract-ctl

port() {
    case "$1" in auth) echo "$AUTH_PORT" ;; mock*) echo "$MOCK_PORT" ;; *) echo "$NOAUTH_PORT" ;; esac
}
hub_modes="" mock_modes=""
for mode in "$@"; do
    case "$mode" in
        noauth | auth) hub_modes+="$mode " ;;
        mock | mock-auth) mock_modes+="$mode " ;;
        *) echo "unknown mode: $mode (noauth | auth | mock | mock-auth)" >&2; exit 2 ;;
    esac
done
# One run at a time: every run uses the same container names and ports, so a
# second run would replace the first one's servers mid-flow. Wait for it.
mkdir -p build
exec 9>build/contract.lock
flock -n 9 || { echo "contract.sh: waiting for another run to finish" >&2; flock 9; }
cleanup() { docker rm -f "$SIM" "$MOCK" >/dev/null 2>&1 || true; }
cleanup
trap cleanup EXIT

# The mock: no latency, discovery finishes at once, and a remote scan keeps
# scanning until the contract answers it (like hub-sim's /sim/radio/found).
if [ -n "$mock_modes" ]; then
    docker compose run -d --name "$MOCK" -T -e HUBBO_MOCK_PORT="$MOCK_PORT" -e HUBBO_MOCK_LATENCY=0 \
        -e HUBBO_MOCK_DISCOVER_MS=0 -e HUBBO_MOCK_PAIR_MS=600000 web mock >/dev/null
fi

docker build -q -t "$IMAGE" -f api/tools/lint.Dockerfile api/tools >/dev/null

if [ -n "$hub_modes" ]; then
    # Its own build directory: build/native is shared with other builds running
    # at the same time, and a half-rebuilt tree can link inconsistent objects.
    docker compose run --rm -T toolchain bash -c '
        [ -f build/contract/build.ninja ] || cmake -S . -B build/contract -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
        cmake --build build/contract --target hub-sim' >/dev/null

    rm -rf "$CTL"
    supervisors=""
    for mode in $hub_modes; do
        mkdir -p "$CTL/$mode"
        flag=""
        [ "$mode" = auth ] && flag="--auth"
        supervisors+="supervise $mode $(port "$mode") '$flag' & "
    done

    # Fresh state: each server gets an empty temporary data directory.
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
fi

run_contract() {  # mode; hub-sim modes get their supervisor directory (the restart checks)
    local ctl=()
    [ -d "$CTL/$1" ] && ctl=(-v "$PWD/$CTL/$1":/ctl)
    docker run --rm --network host --user "$(id -u):$(id -g)" -e HOME=/tmp -e PYTHONDONTWRITEBYTECODE=1 \
        -v "$PWD":/work:ro "${ctl[@]}" -w /work "$IMAGE" \
        python -P api/tools/contract.py --base "http://127.0.0.1:$(port "$1")" --mode "$1" ${ctl:+--ctl /ctl}
}

status=0
for mode in $hub_modes $mock_modes; do run_contract "$mode" || status=1; done
exit $status
