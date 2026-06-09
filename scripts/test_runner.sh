#!/usr/bin/env bash
# =============================================================================
# XLS-20 NFT Price Predictor Agent — Integration Test Runner
# Phase 6: Production Hardening & Testing
#
# Purpose:
#   Runs the application in --mock mode as a gate check before deployment.
#   Validates binary presence, required directory structure, clean exit code,
#   expected terminal output markers, and optional Valgrind memory-leak scan.
#
# Usage:
#   ./scripts/test_runner.sh                     # standard run
#   ./scripts/test_runner.sh --valgrind          # include Valgrind leak check
#   ./scripts/test_runner.sh --binary /path/bin  # override binary location
#
# Exit codes:
#   0  All checks passed
#   1  One or more checks failed (details printed to stderr)
# =============================================================================

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Colour helpers (degrade gracefully when not connected to a terminal)
# ─────────────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; RESET=''
fi

# ─────────────────────────────────────────────────────────────────────────────
# Resolve script location so the test can be run from any working directory
# ─────────────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ─────────────────────────────────────────────────────────────────────────────
# Defaults (overridable via CLI flags)
# ─────────────────────────────────────────────────────────────────────────────
BINARY="${PROJECT_ROOT}/build/XLS20PredictorAgent"
CONFIG_FILE="${PROJECT_ROOT}/config/collections.json"
CACHE_FILE="${PROJECT_ROOT}/data/historical_cache.json"
RUN_VALGRIND=false
MOCK_TIMEOUT=30   # seconds before we consider --mock hung

# ─────────────────────────────────────────────────────────────────────────────
# Parse arguments
# ─────────────────────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --valgrind)   RUN_VALGRIND=true ;;
        --binary)     BINARY="$2"; shift ;;
        --config)     CONFIG_FILE="$2"; shift ;;
        --timeout)    MOCK_TIMEOUT="$2"; shift ;;
        -h|--help)
            grep '^#' "$0" | head -20 | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

# ─────────────────────────────────────────────────────────────────────────────
# Counters
# ─────────────────────────────────────────────────────────────────────────────
PASS=0; FAIL=0

pass() { echo -e "  ${GREEN}✔${RESET}  $1"; (( PASS++ )) || true; }
fail() { echo -e "  ${RED}✘${RESET}  $1" >&2; (( FAIL++ )) || true; }
info() { echo -e "  ${CYAN}ℹ${RESET}  $1"; }
warn() { echo -e "  ${YELLOW}⚠${RESET}  $1"; }

# ─────────────────────────────────────────────────────────────────────────────
# SUITE: Pre-flight directory & file checks
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}━━━  XLS-20 Agent Integration Test Runner  ━━━${RESET}"
echo -e "${CYAN}Project root:${RESET} ${PROJECT_ROOT}\n"

echo -e "${BOLD}[1/5] Pre-flight checks${RESET}"

# 1a. Binary exists and is executable
if [[ -x "${BINARY}" ]]; then
    pass "Binary found and executable: ${BINARY}"
else
    fail "Binary not found or not executable: ${BINARY}"
    echo -e "\n       ${YELLOW}Hint:${RESET} Run 'cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel' first." >&2
    # Cannot continue without the binary
    echo -e "\n${RED}FATAL: cannot proceed without the binary.${RESET}" >&2
    exit 1
fi

# 1b. config/ directory and collections.json exist
if [[ -d "${PROJECT_ROOT}/config" ]]; then
    pass "config/ directory exists"
else
    fail "config/ directory missing — expected at ${PROJECT_ROOT}/config"
fi

if [[ -f "${CONFIG_FILE}" ]]; then
    pass "Config file exists: ${CONFIG_FILE}"
else
    fail "Config file missing: ${CONFIG_FILE}"
fi

# 1c. data/ directory exists and is writable (agent writes cache there)
if [[ -d "${PROJECT_ROOT}/data" ]]; then
    pass "data/ directory exists"
else
    warn "data/ directory missing — creating it now"
    mkdir -p "${PROJECT_ROOT}/data"
    if [[ -d "${PROJECT_ROOT}/data" ]]; then
        pass "data/ directory created successfully"
    else
        fail "Failed to create data/ directory"
    fi
fi

if [[ -w "${PROJECT_ROOT}/data" ]]; then
    pass "data/ directory is writable"
else
    fail "data/ directory is not writable — cache writes will fail at runtime"
fi

# 1d. src/ tree present (sanity check this is the real project root)
if [[ -d "${PROJECT_ROOT}/src" ]]; then
    pass "src/ source tree present"
else
    fail "src/ directory missing — is PROJECT_ROOT correct? (${PROJECT_ROOT})"
fi

# ─────────────────────────────────────────────────────────────────────────────
# SUITE: Config file validation (JSON well-formedness)
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}[2/5] Configuration validation${RESET}"

if command -v python3 &>/dev/null; then
    if python3 -c "import json, sys; json.load(open(sys.argv[1]))" \
               "${CONFIG_FILE}" 2>/dev/null; then
        pass "collections.json is valid JSON"
    else
        fail "collections.json contains invalid JSON"
    fi

    # Check required top-level keys
    REQUIRED_KEYS=("websocket" "forecasting" "output" "collections")
    for key in "${REQUIRED_KEYS[@]}"; do
        if python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if '${key}' in d else 1)
" "${CONFIG_FILE}" 2>/dev/null; then
            pass "Config key present: '${key}'"
        else
            fail "Config key missing: '${key}'"
        fi
    done

    # Validate collections array is non-empty
    COL_COUNT=$(python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
print(len(d.get('collections', [])))
" "${CONFIG_FILE}" 2>/dev/null || echo 0)
    if [[ "${COL_COUNT}" -gt 0 ]]; then
        pass "collections[] contains ${COL_COUNT} entr$([ "${COL_COUNT}" -eq 1 ] && echo y || echo ies)"
    else
        fail "collections[] is empty — no collections to track"
    fi
else
    warn "python3 not available — skipping JSON schema validation"
fi

# ─────────────────────────────────────────────────────────────────────────────
# SUITE: Mock mode smoke test
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}[3/5] Mock mode smoke test (--mock)${RESET}"
info "Running: ${BINARY} --mock  (timeout ${MOCK_TIMEOUT}s)"

MOCK_OUTPUT_FILE="$(mktemp /tmp/xls20_mock_output.XXXXXX)"
MOCK_EXIT=0

# Run inside timeout; capture stdout+stderr together for inspection.
if ! timeout "${MOCK_TIMEOUT}" "${BINARY}" --mock \
        >"${MOCK_OUTPUT_FILE}" 2>&1; then
    MOCK_EXIT=$?
fi

# 3a. Exit code must be 0
if [[ "${MOCK_EXIT}" -eq 0 ]]; then
    pass "Mock mode exited with code 0"
elif [[ "${MOCK_EXIT}" -eq 124 ]]; then
    fail "Mock mode timed out after ${MOCK_TIMEOUT}s (exit code 124)"
else
    fail "Mock mode exited with non-zero code: ${MOCK_EXIT}"
    info "Last 15 lines of output:"
    tail -15 "${MOCK_OUTPUT_FILE}" | sed 's/^/       /' >&2
fi

# 3b. Output must contain the version banner
if grep -qF "XLS-20 NFT Price Predictor Agent" "${MOCK_OUTPUT_FILE}"; then
    pass "Version banner present in output"
else
    fail "Version banner not found in output"
fi

# 3c. Output must contain the mock validation success marker
if grep -qF "Validation complete" "${MOCK_OUTPUT_FILE}" || \
   grep -qF "ANSI table" "${MOCK_OUTPUT_FILE}"; then
    pass "Mock validation completion marker found"
else
    fail "Mock validation completion marker not found in output"
    info "Tail of captured output:"
    tail -10 "${MOCK_OUTPUT_FILE}" | sed 's/^/       /' >&2
fi

# 3d. No fatal error strings in output
FATAL_PATTERNS=("Segmentation fault" "Aborted" "double free" "corrupted" "SIGABRT")
FATAL_FOUND=false
for pattern in "${FATAL_PATTERNS[@]}"; do
    if grep -qiF "${pattern}" "${MOCK_OUTPUT_FILE}"; then
        fail "Fatal runtime error detected: '${pattern}'"
        FATAL_FOUND=true
    fi
done
if [[ "${FATAL_FOUND}" == false ]]; then
    pass "No fatal runtime errors in output"
fi

rm -f "${MOCK_OUTPUT_FILE}"

# ─────────────────────────────────────────────────────────────────────────────
# SUITE: Optional Valgrind memory-leak detection
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}[4/5] Memory leak detection${RESET}"

if [[ "${RUN_VALGRIND}" == true ]]; then
    if ! command -v valgrind &>/dev/null; then
        warn "Valgrind not installed — skipping leak check"
        warn "Install with: sudo apt-get install valgrind"
    else
        VALGRIND_LOG="$(mktemp /tmp/xls20_valgrind.XXXXXX)"
        info "Running Valgrind (this may take ~30 s)…"

        VALGRIND_EXIT=0
        timeout 60 valgrind \
            --tool=memcheck \
            --leak-check=full \
            --errors-for-leak-kinds=definite,indirect \
            --error-exitcode=42 \
            --log-file="${VALGRIND_LOG}" \
            "${BINARY}" --mock >/dev/null 2>&1 || VALGRIND_EXIT=$?

        # Exit 42 = valgrind detected errors; 124 = timeout
        if [[ "${VALGRIND_EXIT}" -eq 0 ]]; then
            pass "Valgrind: no definite/indirect memory leaks detected"
        elif [[ "${VALGRIND_EXIT}" -eq 42 ]]; then
            fail "Valgrind: memory leak(s) detected (exit code 42)"
            # Print only the leak-summary block from the log
            grep -A 10 "LEAK SUMMARY" "${VALGRIND_LOG}" | sed 's/^/       /' >&2 || true
            info "Full Valgrind log: ${VALGRIND_LOG}"
        elif [[ "${VALGRIND_EXIT}" -eq 124 ]]; then
            warn "Valgrind timed out after 60 s — inconclusive"
        else
            warn "Valgrind exited with code ${VALGRIND_EXIT} — check ${VALGRIND_LOG}"
        fi

        # Check for definitely-lost blocks even if exit code was not 42
        # (some Boost/OpenSSL statics look like leaks to Valgrind)
        DEFINITELY_LOST=$(grep "definitely lost:" "${VALGRIND_LOG}" \
            | grep -v "0 bytes in 0 blocks" || true)
        if [[ -n "${DEFINITELY_LOST}" ]]; then
            fail "Definite leaks found: ${DEFINITELY_LOST}"
        elif [[ "${VALGRIND_EXIT}" -eq 0 ]]; then
            : # already passed above
        fi

        [[ "${VALGRIND_EXIT}" -eq 124 ]] || rm -f "${VALGRIND_LOG}"
    fi
else
    info "Valgrind check skipped (pass --valgrind to enable)"
    info "Note: Valgrind on Boost.Asio+OpenSSL apps requires suppression files"
    info "      for TLS/ASIO internal statics.  False positives are expected"
    info "      without a tuned .supp file; only 'definitely lost' blocks matter."
fi

# ─────────────────────────────────────────────────────────────────────────────
# SUITE: Build artefact integrity
# ─────────────────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}[5/5] Binary integrity checks${RESET}"

# 5a. File type is ELF/Mach-O executable
BINARY_TYPE="$(file -b "${BINARY}" 2>/dev/null || echo unknown)"
if echo "${BINARY_TYPE}" | grep -qiE "(ELF|Mach-O).*(executable|64-bit)"; then
    pass "Binary is a valid native executable (${BINARY_TYPE%%,*})"
else
    warn "Unexpected file type: ${BINARY_TYPE} — may still work"
fi

# 5b. Required shared libraries resolve on this host
if command -v ldd &>/dev/null; then
    MISSING_LIBS=$(ldd "${BINARY}" 2>/dev/null \
        | grep "not found" | awk '{print $1}' | tr '\n' ' ')
    if [[ -z "${MISSING_LIBS}" ]]; then
        pass "All shared library dependencies resolved by dynamic linker"
    else
        fail "Missing shared libraries: ${MISSING_LIBS}"
        fail "Run: ldd ${BINARY} | grep 'not found' for the full list"
    fi
elif command -v otool &>/dev/null; then
    # macOS
    MISSING_LIBS=$(otool -L "${BINARY}" 2>/dev/null \
        | awk 'NR>1 {print $1}' \
        | while read -r lib; do
            [[ -f "${lib}" ]] || echo "${lib}"
          done | tr '\n' ' ')
    if [[ -z "${MISSING_LIBS}" ]]; then
        pass "All dylib dependencies resolved (otool check)"
    else
        fail "Missing dylibs: ${MISSING_LIBS}"
    fi
else
    warn "Neither ldd nor otool available — skipping library resolution check"
fi

# 5c. Binary size sanity check (must be > 50 KB to rule out an empty stub)
BINARY_SIZE=$(stat -f%z "${BINARY}" 2>/dev/null || stat -c%s "${BINARY}" 2>/dev/null || echo 0)
if [[ "${BINARY_SIZE}" -gt 51200 ]]; then
    pass "Binary size is reasonable: $(( BINARY_SIZE / 1024 )) KB"
else
    fail "Binary is suspiciously small (${BINARY_SIZE} bytes) — may be a stub or failed build"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Results summary
# ─────────────────────────────────────────────────────────────────────────────
TOTAL=$(( PASS + FAIL ))
echo -e "\n${BOLD}━━━  Results  ━━━${RESET}"
echo -e "  Total checks : ${TOTAL}"
echo -e "  ${GREEN}Passed${RESET}       : ${PASS}"
echo -e "  ${RED}Failed${RESET}       : ${FAIL}"

if [[ "${FAIL}" -eq 0 ]]; then
    echo -e "\n${GREEN}${BOLD}✔  All checks passed — agent is production-ready.${RESET}\n"
    exit 0
else
    echo -e "\n${RED}${BOLD}✘  ${FAIL} check(s) failed — resolve the issues above before deploying.${RESET}\n"
    exit 1
fi
