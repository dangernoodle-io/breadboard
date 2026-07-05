PIO ?= pio

.PHONY: help all check lint cppcheck test-py test coverage smoke clean

help: ## Show available targets
	@grep -E '^[a-zA-Z_%-]+:.*##' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

all: cppcheck test-py test ## Fast subset: static analysis + py-tests + host tests (lint runs per firmware build via BB_LINT_ON_BUILD)

check: lint cppcheck ## Forbidden-pattern lint + static analysis (cppcheck)

lint: ## Forbidden-pattern lint (also enforced on every firmware build via BB_LINT_ON_BUILD)
	python3 scripts/bbtool.py lint --root . --profile library

cppcheck: ## Static analysis (cppcheck)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=redundantAssignment components/; \
	else \
		echo "cppcheck not found, skipping static analysis"; \
	fi

test-py: ## Python tooling tests (bbtool + bbdevice)
	python3 -m unittest discover -s scripts/bbtool/tests
	python3 -m unittest discover -s scripts/bbdevice/tests -t scripts

test: ## Run host unit tests
	$(PIO) test -e native

coverage: test ## Coverage report (gcovr); per-file branch detail aids debugging when Coveralls flags drops
	gcovr --root . --filter 'components/' \
	    --filter 'platform/espidf/bb_cache/' \
	    --filter 'platform/host/bb_cache/' \
	    --filter 'platform/espidf/bb_cache_reactive/' \
	    --filter 'platform/host/bb_cache_reactive/' \
	    --filter 'platform/espidf/bb_init/' \
	    --filter 'platform/host/bb_init/' \
	    --filter 'platform/host/bb_kv/' \
	    --filter 'platform/host/bb_mdns_cache/' \
	    --filter 'platform/host/bb_str/' \
	    --filter 'platform/host/bb_scalar/' \
	    --filter 'platform/host/bb_num/' \
	    --filter 'platform/host/bb_fmt/' \
	    --exclude-throw-branches \
	    --exclude-unreachable-branches \
	    --exclude-directories '\.claude' \
	    --merge-mode-functions=merge-use-line-max \
	    --txt-metric branch \
	    --print-summary \
	    --coveralls gcovr-coveralls.json \
	    --txt

# r4_wifis3 / uno_cc3000 excluded from aggregate + CI pending arm64 toolchain fix (see backlog); use their individual targets locally
smoke: smoke-elecrow-p4-hmi7 smoke-esp32 smoke-esp32-cache-sweep smoke-esp32c3 smoke-tdongle

smoke-elecrow-p4-hmi7: ## Build smoke example for Elecrow CrowPanel P4 HMI 7.0 (with display)
	$(PIO) run -d examples/smoke -e elecrow-p4-hmi7

smoke-esp32: ## Build smoke example for classic ESP32-D0 / WROOM-32
	$(PIO) run -d examples/smoke -e esp32

smoke-esp32-cache-sweep: ## Build smoke with CONFIG_BB_CACHE_SWEEP_ENABLE=y (bb_cache age-out sweep compile gate)
	$(PIO) run -d examples/smoke -e esp32-cache-sweep

smoke-esp32-boot-progress: ## Build smoke with BB_OTA_BOOT_PROGRESS_HTTP=y (gated path compile gate)
	$(PIO) run -d examples/smoke -e esp32-boot-progress

smoke-esp32-boot-status: ## Build smoke with BB_OTA_BOOT_STATUS_HTTP=y (on-demand status routes compile gate)
	$(PIO) run -d examples/smoke -e esp32-boot-status

smoke-esp32-autofan: ## Build smoke with BB_FAN_AUTOFAN=y (autofan compile gate)
	$(PIO) run -d examples/smoke -e esp32-autofan

smoke-esp32c3: ## Build smoke example for ESP32-C3-DevKitM-1
	$(PIO) run -d examples/smoke -e esp32c3

smoke-tdongle: ## Build smoke example for LILYGO T-Dongle-S3
	$(PIO) run -d examples/smoke -e tdongle

smoke-r4_wifis3: ## Build smoke example for Arduino UNO R4 WiFi
	rm -rf examples/smoke/.pio/platform
	cp examples/smoke/include/secrets.h.example examples/smoke/include/secrets.h
	$(PIO) run -d examples/smoke -e r4_wifis3

smoke-uno_cc3000: ## Build smoke example for Arduino UNO + CC3000 shield
	rm -rf examples/smoke/.pio/platform
	cp examples/smoke/include/secrets.h.example examples/smoke/include/secrets.h
	$(PIO) run -d examples/smoke -e uno_cc3000

clean: ## Clean build artifacts
	$(PIO) run -t clean
