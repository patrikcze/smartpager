# SmartPager — common tasks. Run `make` for the list.
#
# Configuration lives in .env (created by `make env`, git-ignored, mode 600).

SHELL := /bin/bash
.DEFAULT_GOAL := help

ENV_FILE  := .env
VENV      := server/.venv
PY        := $(VENV)/bin/python
PIO       ?= $(shell command -v pio || echo $(HOME)/.local/bin/pio)
PORT_DEV  ?= /dev/cu.usbmodem1101
ID        ?= pager-1
COMPOSE   := docker compose
LOAD_ENV  := set -a && . ./$(ENV_FILE) && set +a

.PHONY: help env install test run build up up-public logs-caddy down ingest-token audit rotate-secrets ota-keygen ota-publish restart logs ps health device rotate backup import-db \
        demo fw-build fw-test fw-sim fw-diag fw-flash fw-monitor clean

help: ## Show this help
	@awk 'BEGIN{FS=":.*## "} /^[a-zA-Z_-]+:.*## /{printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)

# ------------------------------------------------------------------ configuration

env: ## Create .env with fresh random tokens (never overwrites an existing file)
	@if [ -f $(ENV_FILE) ]; then echo "$(ENV_FILE) already exists — not touching it"; exit 0; fi; \
	umask 077; \
	tok() { python3 -c 'import secrets; print(secrets.token_urlsafe(32))'; }; \
	sed -e "s|^SMARTPAGER_ADMIN_TOKEN=.*|SMARTPAGER_ADMIN_TOKEN=$$(tok)|" \
	    -e "s|^SMARTPAGER_INGEST_TOKEN=.*|SMARTPAGER_INGEST_TOKEN=$$(tok)|" .env.example > $(ENV_FILE); \
	chmod 600 $(ENV_FILE); \
	echo "created $(ENV_FILE) (mode 600, git-ignored). Admin token: grep ADMIN $(ENV_FILE)"

$(ENV_FILE):
	@echo "missing $(ENV_FILE) — run 'make env' first" >&2; exit 1

# ------------------------------------------------------------------ native server

$(PY):
	cd server && (command -v uv >/dev/null && uv venv -q --python 3.13 .venv || python3 -m venv .venv)
	cd server && (command -v uv >/dev/null && uv pip install -q -e '.[dev]' --python .venv/bin/python || .venv/bin/pip install -q -e '.[dev]')

install: $(PY) ## Create the Python venv and install server dependencies

test: $(PY) ## Run backend tests
	cd server && .venv/bin/pytest -q

run: $(PY) $(ENV_FILE) ## Run the server natively in the foreground (single process, as required)
	@$(LOAD_ENV) && mkdir -p "$$(dirname "$${SMARTPAGER_DB:-./data/smartpager.db}")" && \
	echo "SmartPager on http://$${SMARTPAGER_BIND:-127.0.0.1}:$${SMARTPAGER_PORT:-8080}" && \
	exec $(VENV)/bin/uvicorn --factory app.main:create_app --app-dir server \
	  --host "$${SMARTPAGER_BIND:-127.0.0.1}" --port "$${SMARTPAGER_PORT:-8080}" \
	  --workers 1 --no-access-log --timeout-graceful-shutdown 5

# ------------------------------------------------------------------ docker (production)

build: ## Build the server image
	$(COMPOSE) build

up: $(ENV_FILE) ## Start the server container in the background and wait until healthy
	$(COMPOSE) up -d --build smartpager
	@for i in $$(seq 1 30); do \
	  s=$$(docker inspect -f '{{.State.Health.Status}}' "$$($(COMPOSE) ps -q smartpager)" 2>/dev/null); \
	  [ "$$s" = healthy ] && { echo "smartpager is healthy"; exit 0; }; sleep 2; done; \
	echo "container did not become healthy — see 'make logs'" >&2; exit 1

up-public: $(ENV_FILE) ## Start server + Caddy HTTPS (Let's Encrypt) for SMARTPAGER_DOMAIN
	@$(LOAD_ENV) && test -n "$$SMARTPAGER_DOMAIN" -a -n "$$ACME_EMAIL" || \
	  { echo "set SMARTPAGER_DOMAIN and ACME_EMAIL in $(ENV_FILE) first" >&2; exit 1; }
	@$(LOAD_ENV) && [ "$${SMARTPAGER_BIND:-127.0.0.1}" = 127.0.0.1 ] || \
	  echo "warning: SMARTPAGER_BIND=$$SMARTPAGER_BIND also exposes plain HTTP on port $${SMARTPAGER_PORT:-8080}; use 127.0.0.1 when public" >&2
	$(MAKE) up
	$(COMPOSE) --profile public up -d caddy
	@$(LOAD_ENV) && echo "Caddy is requesting a certificate for $$SMARTPAGER_DOMAIN — check: make logs-caddy"

logs-caddy: ## Follow Caddy (HTTPS proxy) logs
	$(COMPOSE) --profile public logs -f --tail=100 caddy

down: ## Stop the containers (data volume is kept)
	$(COMPOSE) --profile public --profile email down

restart: ## Restart the server container
	$(COMPOSE) restart smartpager

logs: ## Follow server logs
	$(COMPOSE) logs -f --tail=100 smartpager

ps: ## Show container status
	$(COMPOSE) ps

health: $(ENV_FILE) ## Query /health on the configured port
	@$(LOAD_ENV) && curl -sf "http://127.0.0.1:$${SMARTPAGER_PORT:-8080}/health" && echo

device: ## Register a device in the container and print its token once (ID=pager-1)
	$(COMPOSE) exec smartpager smartpager-admin device-create $(ID)

rotate: ## Issue a new token for a device in the container (ID=pager-1)
	$(COMPOSE) exec smartpager smartpager-admin device-rotate $(ID)

backup: ## Consistent SQLite snapshot of the container database into ./backups/
	@mkdir -p backups && f=smartpager-$$(date +%Y%m%d-%H%M%S).db && \
	$(COMPOSE) exec -T smartpager python -c "import sqlite3; s=sqlite3.connect('/data/smartpager.db'); d=sqlite3.connect('/data/$$f'); s.backup(d); d.close()" && \
	docker cp "$$($(COMPOSE) ps -q smartpager):/data/$$f" backups/$$f && \
	$(COMPOSE) exec -T smartpager rm -f /data/$$f && chmod 600 backups/$$f && echo "backups/$$f"

import-db: ## Replace the container database with DB=path/to.db (stops the server first)
	@test -n "$(DB)" -a -f "$(DB)" || { echo "usage: make import-db DB=path/to/smartpager.db" >&2; exit 1; }
	@# Snapshot on the host first: a WAL database can't be opened read-only through a single-file
	@# bind mount (SQLite needs its -wal/-shm side files), and the snapshot is consistent.
	@snap=$$(mktemp -t smartpager-import) && trap 'rm -f "$$snap"' EXIT && \
	python3 -c "import sqlite3,sys; s=sqlite3.connect(sys.argv[1]); d=sqlite3.connect(sys.argv[2]); s.backup(d); d.execute('PRAGMA journal_mode=DELETE'); d.close()" "$(DB)" "$$snap" && \
	chmod 644 "$$snap" && \
	$(COMPOSE) stop smartpager && \
	$(COMPOSE) run --rm --no-deps -v "$$snap:/import/source.db:ro" --entrypoint python smartpager -c \
	  "import os,shutil; [os.remove(p) for p in ('/data/smartpager.db','/data/smartpager.db-wal','/data/smartpager.db-shm') if os.path.exists(p)]; shutil.copyfile('/import/source.db','/data/smartpager.db'); print('database imported')"
	$(MAKE) up

ingest-token: ## Create a named, revocable ingest token for one integration (NAME=grafana)
	@test -n "$(NAME)" || { echo "usage: make ingest-token NAME=grafana" >&2; exit 1; }
	$(COMPOSE) exec smartpager smartpager-admin ingest-create "$(NAME)"

audit: ## Show recent admin/security events
	$(COMPOSE) exec smartpager smartpager-admin audit --limit 40

rotate-secrets: $(ENV_FILE) ## New admin + shared ingest tokens in .env (old .env kept as .env.bak-*) and restart
	@cp -p $(ENV_FILE) $(ENV_FILE).bak-$$(date +%Y%m%d-%H%M%S) && \
	tok() { python3 -c 'import secrets; print(secrets.token_urlsafe(32))'; }; \
	sed -i.tmp -e "s|^SMARTPAGER_ADMIN_TOKEN=.*|SMARTPAGER_ADMIN_TOKEN=$$(tok)|" \
	  -e "s|^SMARTPAGER_INGEST_TOKEN=..*|SMARTPAGER_INGEST_TOKEN=$$(tok)|" $(ENV_FILE) && rm -f $(ENV_FILE).tmp && \
	chmod 600 $(ENV_FILE) && echo "rotated; update scripts that used the shared ingest token"
	$(COMPOSE) up -d smartpager

# ------------------------------------------------------------------ signed OTA updates

ota-keygen: ## Create the OTA signing key (secrets/, never committed) and its firmware header
	python3 tools/ota.py keygen

ota-publish: ## Build, sign and publish the current firmware for OTA (server must be running)
	cd firmware && $(PIO) run -e tdisplays3
	rm -rf build/ota && python3 tools/ota.py sign firmware/.pio/build/tdisplays3/firmware.bin build/ota
	@cid=$$($(COMPOSE) ps -q smartpager); \
	if [ -n "$$cid" ]; then \
	  $(COMPOSE) exec -T smartpager mkdir -p /data/firmware && \
	  docker cp build/ota/firmware.bin "$$cid:/data/firmware/firmware.bin" && \
	  docker cp build/ota/manifest.json "$$cid:/data/firmware/manifest.json" && \
	  echo "published to the container; pagers offer it in Settings → Update"; \
	else \
	  $(LOAD_ENV) && d=$$(dirname "$${SMARTPAGER_DB:-./data/smartpager.db}")/firmware && mkdir -p "$$d" && \
	  cp build/ota/firmware.bin build/ota/manifest.json "$$d/" && echo "published to $$d (native server)"; \
	fi

demo: $(ENV_FILE) ## Run the protocol demo (needs DEVICE_TOKEN in the environment)
	@$(LOAD_ENV) && SMARTPAGER_URL=http://127.0.0.1:$${SMARTPAGER_PORT:-8080} scripts/demo.sh

# ------------------------------------------------------------------ firmware

fw-build: ## Compile the pager firmware
	cd firmware && $(PIO) run -e tdisplays3

fw-test: ## Run firmware host unit tests
	cd firmware && $(PIO) test -e native

fw-sim: ## Render UI screenshots into docs/screenshots
	cd firmware && $(PIO) run -e sim && .pio/build/sim/program ../docs/screenshots

fw-diag: ## Flash the hardware diagnostic image (PORT_DEV=...)
	cd firmware && $(PIO) run -e diag -t upload --upload-port $(PORT_DEV)

fw-flash: ## Flash the pager firmware (PORT_DEV=...)
	cd firmware && $(PIO) run -e tdisplays3 -t upload --upload-port $(PORT_DEV)

fw-monitor: ## Open the serial console
	cd firmware && $(PIO) device monitor -p $(PORT_DEV)

clean: ## Remove build outputs (keeps .env, data and backups)
	rm -rf firmware/.pio/build server/build server/*.egg-info
