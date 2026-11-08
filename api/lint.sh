#!/usr/bin/env bash
# Lint the OpenAPI spec and validate fixtures, in Docker (nothing on the host).
#
#   api/lint.sh        check: fixtures up to date, spec valid and matching core,
#                      fixtures match the spec
#   api/lint.sh gen    regenerate api/fixtures from api/tools/gen_fixtures.py
set -euo pipefail

cd "$(dirname "$0")/.."
IMAGE=hubbo-api-lint
mode="${1:-check}"

case "$mode" in
    gen) script='python -I api/tools/gen_fixtures.py' ;;
    check) script='python -I api/tools/gen_fixtures.py --check && python -I api/tools/validate.py' ;;
    *) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

# Cached after the first build; rebuilds only if requirements.txt changes.
docker build -q -t "$IMAGE" -f api/tools/lint.Dockerfile api/tools >/dev/null

# Run as the host user so generated fixtures stay yours.
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e PYTHONDONTWRITEBYTECODE=1 \
    -v "$PWD":/work -w /work "$IMAGE" sh -c "$script"
