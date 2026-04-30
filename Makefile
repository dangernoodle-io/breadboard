PIO ?= pio

.PHONY: help check test coverage smoke clean

help: ## Show available targets
	@grep -E '^[a-zA-Z_%-]+:.*##' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

check: ## Static analysis (cppcheck)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=redundantAssignment components/; \
	else \
		echo "cppcheck not found, skipping static analysis"; \
	fi

test: ## Run host unit tests
	$(PIO) test -e native

coverage: test ## Coverage report (gcovr)
	gcovr --root . --filter 'components/' \
	    --exclude-throw-branches \
	    --exclude-unreachable-branches \
	    --print-summary --coveralls gcovr-coveralls.json

smoke: smoke-elecrow-p4-hmi7 smoke-esp32 smoke-esp32c3 smoke-r4_wifis3 smoke-tdongle smoke-uno_cc3000

smoke-elecrow-p4-hmi7: ## Build smoke example for Elecrow CrowPanel P4 HMI 7.0 (with display)
	$(PIO) run -d examples/smoke -e elecrow-p4-hmi7

smoke-esp32: ## Build smoke example for classic ESP32-D0 / WROOM-32
	$(PIO) run -d examples/smoke -e esp32

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
