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

smoke: smoke-elecrow-p4-hmi7 smoke-arduino-uno-cc3000 smoke-esp32-wroom-32

smoke-elecrow-p4-hmi7: ## Build examples/elecrow-p4-hmi7
	$(PIO) run -d examples/elecrow-p4-hmi7

smoke-arduino-uno-cc3000: ## Build Arduino Uno + CC3000 example
	cp examples/arduino-uno-cc3000/include/secrets.h.example examples/arduino-uno-cc3000/include/secrets.h
	$(PIO) run -d examples/arduino-uno-cc3000 -e uno

smoke-esp32-wroom-32: ## Build examples/esp32-wroom-32 (classic ESP32-D0 / WROOM-32)
	$(PIO) run -d examples/esp32-wroom-32

clean: ## Clean build artifacts
	$(PIO) run -t clean
