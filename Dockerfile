# ─────────────────────────────────────────────────────────────────────────────
# XLS-20 NFT Price Predictor Agent — Multi-Stage Production Dockerfile
# Phase 6: Production Hardening & Containerization
#
# Build stages:
#   builder  — Ubuntu 24.04 (noble) with full toolchain; compiles Release binary
#   runtime  — Minimal Ubuntu 24.04 with only runtime .so dependencies
#
# Usage:
#   docker build -t xls20-agent:latest .
#   docker run --rm \
#     -v $(pwd)/config:/config:ro \
#     -v $(pwd)/data:/data \
#     xls20-agent:latest /config/collections.json
#
# Volumes:
#   /config   — bind-mount your config/collections.json here (read-only OK)
#   /data     — writable volume for historical_cache.json state
# ─────────────────────────────────────────────────────────────────────────────

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  STAGE 1 — builder                                                          ║
# ║  Full toolchain: GCC-14, CMake 3.28+, Boost, OpenSSL, nlohmann_json        ║
# ╚══════════════════════════════════════════════════════════════════════════════╝
FROM ubuntu:noble AS builder

# Prevent interactive prompts from apt/tzdata during build
ENV DEBIAN_FRONTEND=noninteractive \
    TZ=UTC

# ── System dependencies ───────────────────────────────────────────────────────
# Install in a single layer to minimise cache thrash.
# libboost-system-dev brings all Boost headers + system component.
# nlohmann-json3-dev provides the header-only nlohmann/json.hpp.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        libboost-system-dev \
        libssl-dev \
        nlohmann-json3-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ── Copy source tree ──────────────────────────────────────────────────────────
WORKDIR /build
COPY CMakeLists.txt .
COPY src/           src/

# ── Configure & compile (Release, parallel) ───────────────────────────────────
# -G Ninja is faster than make for incremental builds in CI.
# CMAKE_BUILD_TYPE=Release enables -O3 (as declared in CMakeLists.txt) and
# strips debug sections.  DCMAKE_INSTALL_PREFIX=/install collects the binary
# in a clean directory so the COPY in the runtime stage is surgical.
RUN cmake \
        -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/install \
    && cmake --build build --parallel \
    && cmake --install build --component Runtime --prefix /install \
    || install -D -m 0755 build/XLS20PredictorAgent /install/bin/XLS20PredictorAgent

# ── Collect runtime .so dependencies ─────────────────────────────────────────
# ldd lists every shared library the binary links at runtime.
# We copy each .so into /install/lib so the runtime stage can pick them up
# without installing the full dev packages.
RUN mkdir -p /install/lib && \
    ldd /install/bin/XLS20PredictorAgent \
        | awk '/=>/ { print $3 }' \
        | grep -v '^$' \
        | sort -u \
        | xargs -I{} cp --no-dereference --preserve=links {} /install/lib/ 2>/dev/null || true


# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  STAGE 2 — runtime                                                          ║
# ║  Minimal Ubuntu 24.04 noble with only the runtime .so set + CA certs       ║
# ╚══════════════════════════════════════════════════════════════════════════════╝
FROM ubuntu:noble AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=UTC

# ── Absolute minimal runtime packages ─────────────────────────────────────────
# ca-certificates: required for TLS verification in async_http_* and WSClient.
# libstdc++6 / libgcc-s1: C++ runtime, always needed for any GCC-compiled binary.
# We deliberately do NOT install Boost or OpenSSL dev packages — only the .so
# files copied from the builder stage are present, keeping the image < 100 MB.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
        libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*

# ── Hardened non-root user ────────────────────────────────────────────────────
# Running as UID 10001 (non-root, non-system) follows the principle of least
# privilege.  No shell is assigned to prevent interactive escalation.
RUN groupadd --gid 10001 xls20 \
 && useradd  --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin xls20

# ── Copy build artifacts from builder ────────────────────────────────────────
COPY --from=builder /install/bin/XLS20PredictorAgent /usr/local/bin/XLS20PredictorAgent
COPY --from=builder /install/lib/                    /usr/local/lib/xls20/

# Ensure the dynamic linker can find our bundled libraries.
RUN ldconfig /usr/local/lib/xls20 \
 && chmod 0755 /usr/local/bin/XLS20PredictorAgent

# ── Persistent volume mount points ────────────────────────────────────────────
# /config — bind-mount your config/collections.json here
#           (read-only mount is recommended in production)
# /data   — writable volume for historical_cache.json
#           (must survive container restarts; use a named or host volume)
RUN mkdir -p /config /data \
 && chown xls20:xls20 /config /data \
 && chmod 0750 /config \
 && chmod 0770 /data

# Copy the default config as a fallback so `--mock` works without any volume.
# In production, override by mounting /config at runtime.
COPY config/collections.json /config/collections.json

# Seed an empty cache so the data directory is always valid on first boot.
COPY data/historical_cache.json /data/historical_cache.json

RUN chown xls20:xls20 /config/collections.json /data/historical_cache.json \
 && chmod 0640 /config/collections.json \
 && chmod 0660 /data/historical_cache.json

# ── Runtime metadata ──────────────────────────────────────────────────────────
LABEL org.opencontainers.image.title="XLS-20 NFT Price Predictor Agent" \
      org.opencontainers.image.version="1.4.0" \
      org.opencontainers.image.description="Real-time XRP Ledger NFT price prediction with Holt-Winters forecasting, adaptive volatility bands, and async webhook alerting." \
      org.opencontainers.image.source="https://github.com/your-org/xls20-predictor" \
      org.opencontainers.image.licenses="MIT"

USER xls20
WORKDIR /

# ── Health-check ──────────────────────────────────────────────────────────────
# Runs the --mock mode (exits 0 on success) every 60 s as a liveness probe.
# This validates that the binary is intact and all shared libraries resolve
# without requiring a live XRPL network connection.
HEALTHCHECK --interval=60s --timeout=15s --start-period=5s --retries=3 \
    CMD ["/usr/local/bin/XLS20PredictorAgent", "--mock"]

# ── Entrypoint ────────────────────────────────────────────────────────────────
# Default command: run live mode using the mounted config.
# Override at `docker run` time with `--mock`, `--backtest <file>`, etc.
ENTRYPOINT ["/usr/local/bin/XLS20PredictorAgent"]
CMD ["/config/collections.json"]
