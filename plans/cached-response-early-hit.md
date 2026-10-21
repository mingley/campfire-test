# Cached response architecture comparison and early-hit follow-up

The completed controlled run puts Rust **1.45–1.85× ahead of C# in all eight
cached-read cases**, with lower median p99 in every case. All 62,488,810 timed
responses pass the shared validator.

This follows the measured `93e2a906dd4610391e801297b0c0d7fbc6ef1c2a` Rust
baseline and the unchanged C# `3286f07aa8d00b21e749b8c8c03eb3d5ad3c2ce1`.
See [the initial audit](cached-response-audit.md) for resource policy, seeds,
validation, profiling coverage and publication limitations. The scope remains
room, message-page, sidebar and search cached GETs at 16 and 64 connections.

## Why C# was faster

This is principally a pipeline comparison, not a comparison of language syntax,
compiler settings or SQLite engines. The original Rust baseline required 65–86
microseconds of server CPU per warm request. C# needed 41–67 microseconds with
identical two-CPU server allocations. The Rust profile had about 36% in the kit
pipeline and 19–20% in SQLite, including repeated authentication/resource reads.

| Work per warm hit | Previous Rust | C# | This change |
|---|---|---|---|
| Credential signature/decode | Every request | Exact ticket memo, expiry/key ring checked | Exact request admitted only with a signature valid through its reuse horizon |
| Session/user/room SQL | Every request | Generation-scoped positive authority | Completed pipeline authority scoped to database generation and exact request |
| SQLite generation observation | Capture and lookup | One on early hits | One on early hits |
| Routed context, params, fragments and browser checks | Reconstructed each time | Bypassed on early hits | Bypassed only for an identical successfully admitted request |
| Negotiated response body | Reused after controller checks | Reused before routing | Reused before routing |
| Request ID, runtime, SSL and transport | Fresh | Fresh | Fresh; original middleware ordering retained |

Evidence in the comparator is in `PageCacheHitMiddleware`, `PageTicketMemo`,
`PageAuthorityCache`, `PageSessionAuthority`, `DatabaseGeneration` and
`EncodedCacheBody`. C# still observes SQLite on every eligible request and checks
hourly activity eligibility. It does not simply skip authentication based on a
15-second timer. Its dynamic key ring is a different mechanism from Rust's
immutable per-boot `Secrets`.

## Rust admission and safety boundary

A new early-hit layer sits inside SSL/method-override/request-ID middleware and
before the routed action/context. It can reuse only the completed representation
that the ordinary authenticated pipeline has admitted. Misses retain the ordinary
pipeline. No query, header or cookie result is guessed from the URL.

The key hashes length-delimited original URI, all header names and repeated values,
and peer IP; only request tracing is omitted. Repeated header value counts are
explicit: an empty value followed by name-like values cannot impersonate another
header boundary. Identity, cookies, origin/proxy
headers, format, gzip, user agent and frame variants therefore remain isolated.
A forged or changed credential cannot name a previously admitted entry.

Each early lookup executes `PRAGMA data_version` on the same dedicated observer.
Any committed change from local or foreign writers selects a new namespace,
including timestamp-free session/token, user, membership and content edits.
Admission verifies the generation captured before authentication once more, so a
render racing a commit cannot authorize the new namespace.

Authority has both lower and upper clock bounds. The upper bound is the earlier
of 15 seconds and the next hourly activity refresh. Admission verifies the exact
signed cookie at that future bound, using the existing verifier including purpose,
legacy fallback and expiry semantics. Credentials expiring too soon are simply
ineligible; there is no new parser for unverified cookie metadata. A clock rollback
before the authorization snapshot falls back too. Monotonic body TTL still applies
when the application's wall clock is frozen.

Requests carrying encrypted `_campfire_session` data use the kit, preserving flash
and session expiry/consumption. HEAD, validators, ranges, upgrades, request bodies,
method overrides and cache bypass directives use the existing pipeline. Any final
session/credential cookie prevents early admission. Only a numeric `last_room`
preference may be recreated with a fresh 20-calendar-year expiry on an early hit;
a matching incoming preference remains a no-op. Date, runtime, request IDs, HSTS
and secure-cookie flags remain fresh.

Early and ordinary entries share the existing bounded 64 MiB response store.
Fixed-size authority metadata is charged along with body, headers and keys;
retaining another representation never adds an unbounded authority map. Body
buffers are reference counted, though charged conservatively for each entry.
Disabling the response cache disables early reuse too. TTL, compression, seed,
release settings, C# source and validation are unchanged.

## Validation and reproduction

The full Rust workspace gate uses `CAMPFIRE_REQUIRE_SEED=1` and
`CAMPFIRE_REQUIRE_MEDIA_VECTORS=1`, excluding only vendored `html5ever`, as before.
It passes 803 tests with 11 pre-existing ignored, including typed cache-budget
and repeated-header ambiguity regressions. Formatting and workspace/all-target Clippy with `-D warnings`
pass. The C# checkout and its earlier 7,769 passing non-media tests are unchanged.

New assertions prove the four routes actually take the early path, not merely that
they return successful pages. Existing cache tests cover external and local writes,
revocation/token rotation, room access, body validators, gzip/identity/HEAD, flash,
request variants, and delayed-render races. New tests cover short-lived signed
credentials, bodies, peer IP, preference-cookie no-ops, exact authority deadlines,
clock rollback and freshly generated Date/preference expiries.

Build the normal release binary at `b31416040b8392a6ad34aa5d017281884e61b60d`, with
no profiler/allocator features, and retain the preceding `93e2a90` executable. The
C# command and resource/validation policy remain those in the original audit.

```sh
SERVER_CPUS=0-1 LOADGEN_CPUS=2-3 RUBY=ruby3.3 bench/cached-response \
  --target base=/path/to/rust-93e2a90 \
  --target optimized=/path/to/rust-early-hardened \
  --target dotnet=/path/to/dotnet-r2r/campfire --dotnet dotnet \
  --reps 3 --secs 20 --warmup 5 --concs 16,64 --out tmp/bench/early-hit-confirmation
bench/cached-report tmp/bench/early-hit-confirmation
```

## Final timing results

The three-run median throughput is **44.6–84.7% higher than C# in all eight
cached-read cases**, with an equal-weight geometric mean of **62.5% higher
throughput** and **34.0% lower server CPU/request**. Median p99 is lower in every
cell. Even each cell's slowest Rust run exceeds its fastest C# run in this
three-repetition sample. These observations are specific to this shared cloud
machine and workload, not a claim of formal statistical significance or of
language-wide superiority. The desktop publication's absolute rates and private
harness remain outside this reproduction.

Against the preceding Rust main, the geometric mean throughput ratio is **2.46×**
and CPU/request is **59.4% lower**. Encoded response sizes match the Rust baseline
in every cell across all repetitions; no body/compression changes enter the win.
The architecture change closes the previously measured routed-pipeline gap.

All 72 timed samples completed: **62,488,810 validated responses, zero errors or
invalid responses**, maximum generator utilization **70.5%** (85% rejection
threshold). Metadata verifies that the baseline binary is exactly the previous
optimized Rust binary, the C# DLL is unchanged, and the verification revision,
CPU policy and 5-second/20-second timing conditions match the initial audit.

The 5-second discovery run was exploratory and used the pre-hardening binary.
A first long attempt was interrupted after two baseline cells to fix ambiguous
repeated-header grouping, before any candidate comparison, and is marked failed.
Neither run contributes to the final result. No final-run sample was discarded or
replaced. Authoritative raw files are in ignored `tmp/bench/early-hit-confirmation/`,
including binary/seed hashes, source pins and the generated `report.md`.

| Route | Connections | Rust req/s | C# req/s | Rust / C# | CPU µs Rust / C# | p99 ms Rust / C# |
|---|---:|---:|---:|---:|---:|---:|
| room_show | 16 | 54,978 | 29,772 | 1.85× | 35.2 / 58.3 | 0.696 / 2.549 |
| room_show | 64 | 58,906 | 40,749 | 1.45× | 33.0 / 46.4 | 2.615 / 4.175 |
| messages_page | 16 | 65,985 | 36,333 | 1.82× | 29.4 / 48.7 | 0.582 / 1.399 |
| messages_page | 64 | 64,186 | 40,957 | 1.57× | 30.4 / 46.0 | 2.285 / 4.047 |
| sidebar | 16 | 66,998 | 38,420 | 1.74× | 29.0 / 44.7 | 0.597 / 1.326 |
| sidebar | 64 | 74,278 | 50,790 | 1.46× | 26.5 / 37.2 | 2.123 / 3.197 |
| search | 16 | 62,564 | 37,593 | 1.66× | 31.1 / 46.7 | 0.587 / 1.384 |
| search | 64 | 68,041 | 45,030 | 1.51× | 28.7 / 41.9 | 2.261 / 3.451 |

### Before/after ranges and C# details

All 72 timed samples passed route/body validation; three repetitions per cell.

| Route | c | Rust baseline req/s [range] | Rust optimized req/s [range] | Δ req/s | CPU µs baseline → optimized | p99 ms baseline → optimized |
|---|---:|---:|---:|---:|---:|---:|
| room_show | 16 | 23,128 [22,749–23,918] | 54,978 [53,664–60,366] | +137.7% | 84.4 → 35.2 | 1.421 → 0.696 |
| room_show | 64 | 24,938 [23,965–25,328] | 58,906 [57,360–63,009] | +136.2% | 78.1 → 33.0 | 5.043 → 2.615 |
| messages_page | 16 | 22,917 [22,461–23,905] | 65,985 [59,781–66,336] | +187.9% | 84.4 → 29.4 | 1.478 → 0.582 |
| messages_page | 64 | 23,908 [22,702–25,033] | 64,186 [64,107–67,178] | +168.5% | 80.9 → 30.4 | 5.383 → 2.285 |
| sidebar | 16 | 28,220 [24,372–28,707] | 66,998 [65,489–67,293] | +137.4% | 69.2 → 29.0 | 1.240 → 0.597 |
| sidebar | 64 | 28,597 [28,455–30,494] | 74,278 [73,302–75,954] | +159.7% | 68.3 → 26.5 | 4.339 → 2.123 |
| search | 16 | 28,071 [26,400–28,682] | 62,564 [58,261–64,266] | +122.9% | 69.9 → 31.1 | 1.231 → 0.587 |
| search | 64 | 30,097 [29,694–31,688] | 68,041 [60,895–73,845] | +126.1% | 64.8 → 28.7 | 4.371 → 2.261 |

| Route | c | C# ReadyToRun JIT req/s [range] | CPU µs | Median p99 ms | Encoded body bytes |
|---|---:|---:|---:|---:|---:|
| room_show | 16 | 29,772 [29,729–31,068] | 58.3 | 2.549 | 23,950 |
| room_show | 64 | 40,749 [37,983–41,065] | 46.4 | 4.175 | 23,950 |
| messages_page | 16 | 36,333 [33,731–37,994] | 48.7 | 1.399 | 14,705 |
| messages_page | 64 | 40,957 [36,052–42,426] | 46.0 | 4.047 | 14,705 |
| sidebar | 16 | 38,420 [33,928–43,008] | 44.7 | 1.326 | 6,738 |
| sidebar | 64 | 50,790 [42,343–51,749] | 37.2 | 3.197 | 6,738 |
| search | 16 | 37,593 [36,714–40,955] | 46.7 | 1.384 | 9,808 |
| search | 64 | 45,030 [44,489–45,406] | 41.9 | 3.451 | 9,808 |

Total validated timed responses: 62,488,810. Maximum generator utilization: 0.705.

## Diagnostic follow-up

Separate gperftools SIGPROF runs use the two normal binaries, two warmed routes,
16 connections and 5-second windows, excluded from the timing matrix. The profiles
contain 1,883/2,095 samples for baseline room/sidebar and 1,967/2,049 for optimized
room/sidebar. Inclusive stacks containing `adapter::dispatch` account for
49.5%/47.9% of baseline samples and none of the observed optimized samples;
`concerns::` frames account for 16.2%/18.3% before and none observed after. The
SQLite observer remains in optimized stacks (7.0%/7.7% inclusive). These short
warmed windows illustrate the early path; the 20-second timing windows also
include ordinary TTL expiry/misses. Some libc leaves remain unresolved and
sample shares are not precise elapsed CPU measurements. Process CPU in the
validated timing matrix is the quantitative CPU/request evidence. Raw profiles,
folded stacks and symbolized tables are in `tmp/bench/early-hit-cpu/`. The
unchanged C# managed-stack/allocation trace coverage and its limits remain as
reported in the original audit.

The matched, full-rate bundled-jemalloc zero/N experiment uses 100 warm requests
and N=10,000 sidebar requests for both builds. Profiled cumulative allocation
objects/request fall from **255.0817 to 112.0048 (−56.1%)**; profiled bytes/request
fall from **44,466.4336 to 30,075.4848 (−32.4%)**. Arena counts independently give
255.1273 → 112.0860 allocations/request. The instrumented N windows last
21.21/7.37 seconds, respectively, and include naturally different amounts of
background work and TTL expiry. These are aggregate approximate request deltas,
not exact handler counts, retained-memory/RSS measures or comparable C# managed
allocation coverage. SQLite's libc allocations are outside this census.

Raw heaps, allocator JSON/logs and summaries live in
`tmp/bench/early-hit-alloc-base/` and `tmp/bench/early-hit-alloc-optimized/`. Build
with `--features tikv-jemallocator/profiling,tikv-jemallocator/stats`, then invoke
`bench/cached-alloc --binary PATH --requests 10000 --out DIRECTORY` for each
revision; its `_RJEM_MALLOC_CONF` enables the bundled prefixed allocator. None of
these features or profiles are enabled in the timed binaries. The ordinary
release binary is restored for development startup afterward.
