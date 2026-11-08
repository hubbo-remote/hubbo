# Web admin dev image: Node + pnpm for the React UI in web/. Nothing gets
# installed on the host. Runs as a user whose uid/gid match the host user so
# files written into the bind-mounted repo stay yours.
#
# node_modules (and the pnpm store inside it) live in the `web_node_modules`
# named volume, not the repo, so the host never sees Linux-built binaries.
FROM node:22.23.4-bookworm-slim

ARG PNPM_VERSION=10.12.1
RUN npm install -g "pnpm@${PNPM_VERSION}" && npm cache clean --force

# Re-id the image's `node` user to match the host user.
ARG HOST_UID=1000
ARG HOST_GID=1000
RUN groupmod -o -g "${HOST_GID}" node \
 && usermod -o -u "${HOST_UID}" -g "${HOST_GID}" node \
 && mkdir -p /work/web/node_modules \
 && chown -R node:node /work /home/node

ENV npm_config_store_dir=/work/web/node_modules/.pnpm-store \
    CI=true

USER node
WORKDIR /work/web
