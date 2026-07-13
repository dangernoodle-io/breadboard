PIO ?= pio

.PHONY: help all check lint cppcheck docs docs-index-check docs-check fence di-fence size-check size-baseline test-py test coverage smoke smoke-codegen smoke-gen smoke-gen-esp32 smoke-gen-esp32c3 smoke-gen-tdongle smoke-gen-elecrow-p4-hmi7 floor floor-gen floor-codegen clean

help: ## Show available targets
	@grep -E '^[a-zA-Z_%-]+:.*##' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

all: cppcheck test-py test ## Fast subset: static analysis + py-tests + host tests (lint runs per firmware build via BB_LINT_ON_BUILD)

check: lint cppcheck docs-index-check docs-check fence ## Forbidden-pattern lint + static analysis (cppcheck) + docs drift checks + ratchet-fence

lint: smoke-gen floor-gen ## Forbidden-pattern lint (also enforced on every firmware build via BB_LINT_ON_BUILD)
	python3 scripts/bbtool.py lint --root . --profile library

docs: ## Regenerate generated marker regions in component READMEs
	python3 scripts/bbtool.py docs gen

docs-check: docs ## Verify component README marker regions match generated content (no drift)
	git diff --exit-code -- 'components/*/README.md'

docs-index-check: ## Verify components/README.md matches generated content (no drift)
	python3 scripts/gen_components_readme.py --check

fence: ## Ratchet-fence lint over all families (di-legacy today; enforced via `check`)
	python3 scripts/bbtool.py fence --root .

di-fence: ## [alias] DI legacy ratchet-fence only — see `fence`
	python3 scripts/bbtool.py di-fence --root .

size-check: ## Check esp32 smoke build against its committed footprint baseline (needs smoke-esp32 built first; not part of `check`)
	python3 scripts/bbtool.py size --check --target esp32 --build-dir examples/smoke/.pio/build/esp32 --root .

size-baseline: ## Update the esp32 smoke footprint baseline from the current build (needs smoke-esp32 built first)
	python3 scripts/bbtool.py size --update-baseline --target esp32 --build-dir examples/smoke/.pio/build/esp32 --root .

cppcheck: ## Static analysis (cppcheck)
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=redundantAssignment components/; \
	else \
		echo "cppcheck not found, skipping static analysis"; \
	fi

# smoke-gen/floor-gen prerequisites (B1-741 §7): test_lint_rules.py's
# test_real_repo_clean_smoke_and_floor calls the emit-seam-unwired-subscriber
# rule directly against the real repo tree (examples/*/main/generated/), and
# CI's py-tests job runs `make test-py` on a fresh checkout independent of
# `check`/`smoke` -- without regenerating first, that integration test
# false-fails on a fresh checkout the same way `lint` would.
test-py: smoke-gen floor-gen ## Python tooling tests (bbtool + bbdevice)
	python3 -m unittest discover -s scripts/bbtool/tests
	python3 -m unittest discover -s scripts/bbdevice/tests -t scripts

test: ## Run host unit tests (both compile-time BB_LOCK_STATS_ENABLE states)
	$(PIO) test -e native
	$(PIO) test -e native_lock_stats_off

coverage: test ## Coverage report (gcovr); per-file branch detail aids debugging when Coveralls flags drops
	gcovr --root . --filter 'components/' \
	    --filter 'platform/espidf/bb_cache/' \
	    --filter 'platform/host/bb_cache/' \
	    --filter 'platform/espidf/bb_cache_reactive/' \
	    --filter 'platform/host/bb_cache_reactive/' \
	    --filter 'platform/espidf/bb_cache_serialize/' \
	    --filter 'platform/host/bb_cache_serialize/' \
	    --filter 'platform/host/bb_sink_display/' \
	    --filter 'platform/host/bb_cache_routes/' \
	    --filter 'platform/host/bb_mdns_cache/' \
	    --filter 'platform/host/bb_str/' \
	    --filter 'platform/host/bb_scalar/' \
	    --filter 'platform/host/bb_num/' \
	    --filter 'platform/host/bb_fmt/' \
	    --filter 'platform/host/bb_core/bb_clock\.c' \
	    --filter 'platform/host/bb_core/bb_lock\.c' \
	    --filter 'platform/host/bb_core/bb_lock_cond\.c' \
	    --filter 'platform/host/bb_core/bb_lock_impl\.h' \
	    --filter 'platform/host/bb_meminfo/' \
	    --filter 'platform/host/bb_mem_arena/' \
	    --filter 'platform/host/bb_bqueue/' \
	    --filter 'test/test_host/bb_serialize_meta_validate\.c' \
	    --filter 'test/test_host/bb_serialize_meta_openapi\.c' \
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

# Per-board codegen prerequisites for every smoke-<board> target -- regenerate
# smoke's REQUIRES fragment (board-parameterized, B1-747) + composition root
# (bb_app_init.c, deliberately board-invariant -- pinned to the
# smoke_wire_baseline manifest board via --wire-board) from // bbtool:init
# markers. .PHONY (not real file targets) because bbtool codegen's own inputs
# (component headers, bbtool.toml) aren't tracked here; every smoke-<board>
# run regenerates fresh. Output stays gitignored (decision #725).
# `smoke-gen` (no suffix) is the `lint`/`test-py` prerequisite (B1-741 §7,
# alongside floor-gen): a fresh checkout has no examples/*/main/generated/
# (gitignored), so without this, bbtool lint's emit-seam-unwired-subscriber
# rule (B1-740) can't see a codegen-only-emitted setter wire and
# false-positives; esp32_wroom_32 is the canonical board for that purpose.
smoke-gen: smoke-gen-esp32

smoke-gen-esp32:
	python3 scripts/bbtool.py codegen --root . --board esp32_wroom_32 --wire-board smoke_wire_baseline \
	    --components-out examples/smoke/main/generated/bb_autowire_components.cmake \
	    --wire-out examples/smoke/main/generated/bb_app_init.c

smoke-gen-esp32c3:
	python3 scripts/bbtool.py codegen --root . --board esp32_c3_devkitm_1 --wire-board smoke_wire_baseline \
	    --components-out examples/smoke/main/generated/bb_autowire_components.cmake \
	    --wire-out examples/smoke/main/generated/bb_app_init.c

smoke-gen-tdongle:
	python3 scripts/bbtool.py codegen --root . --board lilygo_t_dongle_s3 --wire-board smoke_wire_baseline \
	    --components-out examples/smoke/main/generated/bb_autowire_components.cmake \
	    --wire-out examples/smoke/main/generated/bb_app_init.c

smoke-gen-elecrow-p4-hmi7:
	python3 scripts/bbtool.py codegen --root . --board elecrow_p4_hmi7 --wire-board smoke_wire_baseline \
	    --components-out examples/smoke/main/generated/bb_autowire_components.cmake \
	    --wire-out examples/smoke/main/generated/bb_app_init.c

smoke-elecrow-p4-hmi7: smoke-gen-elecrow-p4-hmi7 ## Build smoke example for Elecrow CrowPanel P4 HMI 7.0 (with display)
	$(PIO) run -d examples/smoke -e elecrow-p4-hmi7

smoke-esp32: smoke-gen-esp32 ## Build smoke example for classic ESP32-D0 / WROOM-32
	$(PIO) run -d examples/smoke -e esp32

smoke-esp32-cache-sweep: smoke-gen-esp32 ## Build smoke with CONFIG_BB_CACHE_SWEEP_ENABLE=y (bb_cache age-out sweep compile gate)
	$(PIO) run -d examples/smoke -e esp32-cache-sweep

smoke-esp32-boot-progress: smoke-gen-esp32 ## Build smoke with BB_OTA_BOOT_PROGRESS_HTTP=y (gated path compile gate)
	$(PIO) run -d examples/smoke -e esp32-boot-progress

smoke-esp32-boot-status: smoke-gen-esp32 ## Build smoke with BB_OTA_BOOT_STATUS_HTTP=y (on-demand status routes compile gate)
	$(PIO) run -d examples/smoke -e esp32-boot-status

smoke-esp32-autofan: smoke-gen-esp32 ## Build smoke with BB_FAN_AUTOFAN=y (autofan compile gate)
	$(PIO) run -d examples/smoke -e esp32-autofan

smoke-esp32c3: smoke-gen-esp32c3 ## Build smoke example for ESP32-C3-DevKitM-1
	$(PIO) run -d examples/smoke -e esp32c3

smoke-tdongle: smoke-gen-tdongle ## Build smoke example for LILYGO T-Dongle-S3
	$(PIO) run -d examples/smoke -e tdongle

smoke-r4_wifis3: ## Build smoke example for Arduino UNO R4 WiFi
	rm -rf examples/smoke/.pio/platform
	cp examples/smoke/include/secrets.h.example examples/smoke/include/secrets.h
	$(PIO) run -d examples/smoke -e r4_wifis3

smoke-uno_cc3000: ## Build smoke example for Arduino UNO + CC3000 shield
	rm -rf examples/smoke/.pio/platform
	cp examples/smoke/include/secrets.h.example examples/smoke/include/secrets.h
	$(PIO) run -d examples/smoke -e uno_cc3000

floor: ## Build the hand-wired floor example for esp32 (no codegen pre-step)
	$(PIO) run -d examples/floor -e esp32

# Codegen-only half of floor-codegen (no PIO build) -- lint prerequisite
# (B1-741 §7): `lint` depends on this (alongside smoke-gen) so
# examples/floor/main/generated/ exists on a fresh checkout (gitignored,
# decision #725) before bbtool lint's emit-seam-unwired-subscriber rule
# (B1-740) scans examples/*/main/ for the codegen-emitted wire.
floor-gen:
	python3 scripts/bbtool.py codegen --root . --board floor \
	    --components-out examples/floor/main/generated/bb_autowire_components.cmake \
	    --wire-out examples/floor/main/generated/bb_app_init.c

floor-codegen: floor-gen ## Regenerate bb_app_init.c from // bbtool:init markers over floor's exact component set, then rebuild floor so it compiles against real bb_log.h prototypes -- proves the codegen path end-to-end; floor's app_main stays hand-wired (bb_app_init() is compiled, never called)
	$(PIO) run -d examples/floor -e esp32

smoke-codegen: smoke-gen ## [alias] Regenerate smoke's composition root (bb_app_init.c) from // bbtool:init markers over SMOKE_REQUIRES, then rebuild smoke esp32 -- entry_espidf.c actually CALLS bb_app_init_early()/bb_app_init() (unlike floor's proof-only wiring), so this is smoke's normal build path now, not a proof -- see smoke-esp32
	$(PIO) run -d examples/smoke -e esp32

clean: ## Clean build artifacts
	$(PIO) run -t clean
	rm -f examples/floor/main/generated/bb_app_init.c examples/floor/main/generated/bb_app_init.cmake examples/floor/main/generated/bb_autowire_components.cmake
	rm -f examples/smoke/main/generated/bb_app_init.c examples/smoke/main/generated/bb_app_init.cmake examples/smoke/main/generated/bb_autowire_components.cmake
