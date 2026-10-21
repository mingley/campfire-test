# Cached HTTP response audit

This records the first optimization stage at `93e2a90`. The
[architectural follow-up](cached-response-early-hit.md) records the current result:
Rust leads C# by 45–85% across all eight tested cached-read cases.


## Scope and reproducibility

Rust baseline: `basecamp/once-campfire-rust` at
`6dae2fd3c1a2dce61ca11beb731bcce2b42b67ad`. C# reference:
`devmobasa/once-campfire-dotnet` at `3286f07aa8d00b21e749b8c8c03eb3d5ad3c2ce1`.
Shared validation: `basecamp/once-campfire-verification` at
`ec02debb3e44f40b8778a6a42db3c4515b2f9eaf`.
Both apps use copies of Rust's default Rails-built parity seed from reference
`90b330024dec3e757c79b6a7e6568f93da8e3148`. No upstream branch is changed.
Each timed cell repeatedly reads one URL as one signed-in user, with no concurrent
writer. It measures warm cached reads rather than many-user chat capacity.

The published C# table uses this exact Rust revision, gzip, warm response caches,
16 and 64 connections, 5 seconds of warmup and 20-second page windows, with three
interleaved repetitions. Its full custom validator and multi-language orchestration
are explicitly unavailable in that repository. The published Rust README uses
different hardware and a different client; its absolute rates cannot be combined
with the C# table. Neither table is a pure language comparison: security, cookies,
rendering and compression differ between implementations.

This cloud machine has five allowed CPU IDs and a four-CPU cgroup quota. The
controlled comparison allocates CPUs 0–1 to each server and 2–3 to the generator;
Rust uses two Tokio workers. The desktop publication allocates four CPU threads to
the server and eight separate threads to the generator. Therefore this is a
reproduction of the cached-read workloads with explicit resource differences,
not a reproduction of the published absolute rates. Native processes avoid Docker
bridge/proxy overhead for both implementations. C# uses its pinned .NET 11 RC2 JIT
configuration with ReadyToRun and the existing semaphore spin limit of 256. Rust
uses Rust 1.98.1 and its unmodified release profile. No CPU target, LTO, panic,
compression, cache-size, TTL or authentication-policy tuning is part of the change.

## Hot paths

Rust's front server forwards private pages into the kit. The adapter parses the
request, cookies and parameters, constructs the context, and enters the fragment
namespace. Snapshot capture observes SQLite before authentication. Normal session
authentication, user/resource reads and request side effects run on every hit.
`response_cache::lookup` hashes the generation, identity, origin, URI and sorted
request headers (excluding validators/tracing), re-observes SQLite under the
observer mutex, and looks up the bounded completed response. The kit still owns
conditional requests, HEAD, cookies and flash; the deflater retains encoding
negotiation. Admission checks the original generation again after rendering.

C# has an earlier `PageCacheHitMiddleware` before its routed browser pipeline.
`PageTicketMemo` reuses exact protected cookie decoding while checking key-ring
identity and expiry. `DatabaseGeneration` executes a retained SQLite statement on
every observation. Generation-scoped, bounded positive authority entries avoid
repeating session/user/room SQL on a warm hit; token, active-user status and hourly
activity eligibility still gate reuse. `PageCacheRequest.Key` and the response
cache keep identity and representation isolation. `EncodedCacheBody` writes an
admitted encoded body through the outer transport. Misses go through ordinary
authentication and the normal endpoint pipeline. Both caches have a 64 MiB
default budget and a 15-second maximum lifetime, but they do different work per hit.

These differences explain why copying C#'s early-return shape would be a much
larger security-sensitive change than retaining a prepared observer statement.
This work keeps Rust's authentication reads and both freshness observations.

The final changes stay below that policy boundary:

1. Use `prepare_cached` for the observer's fixed `PRAGMA data_version` statement.
   Each call still steps and resets SQLite inside the same observer mutex. The
   cached object is the statement, never its observed generation/result.
2. Let the already-boxed adapter future contain the action future directly. Its
   panic boundary remains around polling, including a panic after suspension.
3. Normalize `host_with_port` from one owned authority string, trimming or
   rewriting the port in place. The old code called both `port()` and `host()`,
   reread the forwarded authority and allocated intermediate strings. Default
   ports, leading zeros, overflowing numeric ports, IPv6 and malformed suffixes
   retain their prior results. Forwarded-host selection is unchanged.

## Profiling method and initial evidence

Rust CPU: gperftools SIGPROF sampling at 1 kHz, enabled only around warmed HTTP
windows with `CPUPROFILESIGNAL=12`, symbolized using `bench/lib/cpuprof.py` and
LLVM 19. Room/sidebar baseline profiles have 4,195/3,512 samples. The observer owns
7.5%/7.7% inclusive CPU; SQLite preparation owns about 2.6–2.7% in the sidebar
profile. Request/response plumbing owns about 36%, HTTP about 24%, and SQLite
about 19–20%. Some libc leaf symbols remain unresolved. These are diagnostic runs;
their instrumented timings do not enter the throughput table.

C# uses EventPipe managed-stack sampling plus sampled GC allocation ticks, with
process user+system CPU accounted independently. The initial sidebar diagnostic
has 26,888 allocation events, 517 heap histories and zero lost events; about
9,871 managed bytes per measured response. Strings dominate sampled types.
EventPipe stack samples include parked threads: its top-method percentages are
wall-time samples, not CPU percentages, and must not be compared with SIGPROF
percentages. This first diagnostic used ordinary JIT; the final comparison uses
the repository's ReadyToRun publication settings.

Rust allocation diagnostics must enable the bundled allocator's `profiling` and
`stats` dependency features and use `_RJEM_MALLOC_CONF`. Preloading system jemalloc
does not intercept prefixed Rust allocations. Full-rate profiles and matched
zero/N request runs count Rust allocation requests separately from timing. SQLite
uses libc malloc and is outside this Rust allocator census. The optional Linux/
glibc `bench/lib/malloc-count.c` probe measures those calls separately. Its warmed
deltas are near zero (small negative deltas are startup/background noise); there
is no measured recurring libc heap saving to claim. SQLite's lookaside store can
serve small statement allocations. C# GC ticks are
sampled managed bytes, so the two allocation reports have different coverage.

An intermediate candidate with only changes 1 and 2 added about one net Rust
allocation and 48–62 bytes per sidebar request: rusqlite replaces an `Arc<str>`
cache key on each of the two observations, offset partly by the removed action
box. This prompted change 3, guided by the baseline's `Ctx::url_for` (about 1.9%
inclusive) and `Request::host_with_port` (about 1.6%) samples. The initial timing
attempt was intentionally interrupted before any candidate comparison and is
marked failed/excluded in its raw receipt; final results use a fresh complete run.

## Commands and artifacts

Initialize each repository's `reference` submodule. Build the pinned Rails image,
then `parity/bin/seed build default` (or run its identical seed builder in that
image with the fixture and seed mounts). Install native libvips/FFmpeg dependencies
from the Dockerfiles. Build the shared client with `cargo build --release --locked
--manifest-path ../once-campfire-verification/loadgen/Cargo.toml`.

`bench/cached-response` starts fresh seed copies, signs in, builds independent
SQL-derived contracts with the shared `bench/contracts.rb`, and passes `--validate`
to every warmup and measured request. It rejects errors, invalid bodies, zero
requests and generator utilization at or above 85%. It alternates target order.
Its `--profile` mode is diagnostic only. `bench/cached-alloc` performs matched
allocation runs with identical startup and 100 warm requests. Raw JSON, profiles,
logs, contracts and test binaries belong in ignored `tmp/bench/`, never commits.
Use at least 10,000 measured requests for aggregate allocation deltas; they still
include background work and TTL misses. `bench/align-heap-profile` aligns PIE and
shared-library mappings before cross-process `jeprof --base` subtraction. Raw
unrelocated subtraction can produce impossible negative allocation sites. The
profile totals and arena counts are reported separately; detailed leaf rankings
are not used because prefixed allocator-profiler frames dominate them.

The cloud toolchain activation file is `/workspace/toolchains/activate.sh`.
Use existing checkouts in this isolated task; no Git worktree is needed.

## Allocation results and validation

For 10,000 validated sidebar requests, matched full-rate jemalloc profiles give
262.16 → 255.08 profiled objects per request (about 2.7% fewer) and
44,575 → 44,467 profiled bytes per request (about 0.24% fewer). Arena counters give
262.24 → 255.25 allocation requests per response. These are aggregate deltas,
including the normal 15-second expiry and background work, not exact per-handler
counts or retained-heap/RSS measurements. The optimized instrumented request
window was 22.63 seconds. Diagnostic profiler overhead is excluded from timings.

The C# ReadyToRun traces estimate 10,331 managed bytes per room response and
9,859 per sidebar response, with zero lost events in both traces. These sampled
managed-byte figures have different coverage from the Rust full-rate allocator
figures and do not imply corresponding retained-memory ratios.

Baseline Rust: 795 passed, 11 existing ignored tests. Final Rust: 798 passed,
11 existing ignored tests, with both `CAMPFIRE_REQUIRE_SEED=1` and
`CAMPFIRE_REQUIRE_MEDIA_VECTORS=1`. The final run includes the eight strict media
vector tests. `cargo fmt --all -- --check` and workspace Clippy pass. New tests
cover repeated observer use across schema changes, commits, rollback and a WAL
checkpoint; panic handling after an action suspends; and authority normalization
under HTTP/HTTPS and direct/forwarded hosts.

C#: 7,769 non-media tests passed (5,830 application, 1,323 web, 616 integration).
The initial proxy-related TLS failures disappeared after adding the synthetic
loopback test domains to both uppercase and lowercase proxy bypass variables.
C# Native AOT, its media suite, browser screenshots and Cable saturation were not
run. The timed scope here is the four cached HTTP read workloads, not the
publication's posting or real-time workloads. Posting behavior remains covered
by the application test suites.

## Reproduce the controlled comparison

With a clean working tree, build Rust baseline `6dae2fd` and the candidate from
the existing checkout, copying each executable into distinct ignored paths before
switching revisions. Use `cargo build --release --locked -p campfire` for both;
do not enable allocator profiling for timed binaries. The C# build is:

```sh
dotnet restore src/Campfire.Web --locked-mode -p:PublishReadyToRun=true
dotnet publish src/Campfire.Web -c Release -r linux-x64 --no-restore \
  --self-contained false -p:PublishAot=false -p:PublishReadyToRun=true -o /path/to/jit
```

From the Rust checkout, with no other builds, servers or profilers consuming the
timing CPUs:

```sh
SERVER_CPUS=0-1 LOADGEN_CPUS=2-3 RUBY=ruby3.3 bench/cached-response \
  --target base=/path/to/rust-baseline \
  --target optimized=/path/to/rust-candidate \
  --target dotnet=/path/to/jit/campfire --dotnet dotnet \
  --reps 3 --secs 20 --warmup 5 --concs 16,64 --out tmp/bench/comparison
bench/cached-report tmp/bench/comparison
```

For Rust allocations, build both revisions separately with
`--features tikv-jemallocator/profiling,tikv-jemallocator/stats`, retain their
executables under distinct paths, then use `bench/cached-alloc --binary PATH
--requests 10000 --out tmp/bench/alloc-LABEL`. The optional C-only probe is built
with `cc -shared -fPIC -O2 -o tmp/bench/malloc-count.so bench/lib/malloc-count.c`
and passed as `--libc-counter tmp/bench/malloc-count.so`.

## Timed results

Across the eight route/concurrency cells, the geometric mean of the three-run
median throughput ratios is **+8.2%** and of the median CPU/request ratios is
**−7.3%**. Individual throughput median gains are **5.5–13.4%**. Median p99 improves
in every cell. These are equal-weight cell ratios, not one blended traffic rate.
Several ranges overlap, and three runs on a shared cloud machine do not establish
statistical significance or predict desktop/deployment performance. C# remains
faster on these repeated warm reads; the policy/pipeline differences above remain.

Raw artifacts are under `tmp/bench/cached-response-confirmation/`; metadata records
exact executable hashes, candidate commit `d7a959f`, seed hash, validator revision
and CPU policy. The report below is generated by `bench/cached-report`, which
rejects incomplete, failed, profiled, invalid-response and generator-limited runs.

All 72 timed samples passed route/body validation; three repetitions per cell.

| Route | c | Rust baseline req/s [range] | Rust optimized req/s [range] | Δ req/s | CPU µs baseline → optimized | p99 ms baseline → optimized |
|---|---:|---:|---:|---:|---:|---:|
| room_show | 16 | 20,970 [19,245–21,107] | 22,736 [22,647–23,474] | +8.4% | 92.0 → 85.8 | 1.581 → 1.408 |
| room_show | 64 | 23,153 [20,740–24,047] | 24,427 [24,410–24,978] | +5.5% | 83.5 → 79.3 | 5.235 → 4.963 |
| messages_page | 16 | 21,405 [18,234–21,545] | 22,817 [22,258–23,152] | +6.6% | 90.2 → 84.6 | 1.499 → 1.489 |
| messages_page | 64 | 21,707 [20,613–23,768] | 23,981 [23,756–24,794] | +10.5% | 88.2 → 80.1 | 5.559 → 4.947 |
| sidebar | 16 | 25,298 [24,145–26,520] | 27,316 [26,892–27,324] | +8.0% | 76.8 → 71.3 | 1.353 → 1.232 |
| sidebar | 64 | 26,965 [25,939–28,003] | 28,955 [28,907–28,983] | +7.4% | 72.3 → 67.5 | 4.655 → 4.447 |
| search | 16 | 25,593 [25,244–26,146] | 27,117 [26,692–27,434] | +6.0% | 76.0 → 71.7 | 1.284 → 1.257 |
| search | 64 | 26,688 [26,530–27,920] | 30,274 [25,937–31,538] | +13.4% | 73.0 → 64.7 | 4.675 → 4.155 |

| Route | c | C# ReadyToRun JIT req/s [range] | CPU µs | Median p99 ms | Encoded body bytes |
|---|---:|---:|---:|---:|---:|
| room_show | 16 | 26,838 [23,886–28,014] | 66.7 | 3.151 | 23,936 |
| room_show | 64 | 37,195 [36,487–38,410] | 50.7 | 4.503 | 23,936 |
| messages_page | 16 | 34,320 [32,424–36,504] | 51.5 | 1.474 | 14,704 |
| messages_page | 64 | 39,554 [38,290–40,457] | 47.9 | 4.271 | 14,704 |
| sidebar | 16 | 36,485 [36,201–38,255] | 46.7 | 1.469 | 6,735 |
| sidebar | 64 | 45,298 [41,371–47,468] | 41.4 | 3.645 | 6,735 |
| search | 16 | 36,025 [35,346–36,256] | 48.6 | 1.463 | 9,805 |
| search | 64 | 42,830 [40,495–43,495] | 43.6 | 4.155 | 9,805 |

Total validated timed responses: 41,694,806. Maximum generator utilization: 0.545.

## Architectural follow-up

The figures above describe the first optimization stage. See the
[early-hit follow-up](cached-response-early-hit.md) for the subsequent comparison
of C#’s early authenticated middleware with Rust’s routed pipeline, the bounded
authority reuse change and its new matched results.
