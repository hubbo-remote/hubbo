# Image for api/lint.sh: pinned Python plus the pinned lint packages.
# Rebuilt automatically (layer cache) when requirements.txt changes.
FROM python:3.12.7-slim-bookworm
COPY requirements.txt /requirements.txt
RUN PIP_DISABLE_PIP_VERSION_CHECK=1 pip install -q --no-cache-dir --root-user-action=ignore \
    -r /requirements.txt
