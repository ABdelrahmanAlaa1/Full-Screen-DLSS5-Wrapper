# Compliance with the Rules for AI-Written Code

This document maps every rule of `AI_CODE_RULES.md` to the way the code meets it and to the check that
enforces it. "Compiler" means the warnings-as-errors configuration in `CMakeLists.txt`; "gate" means
`gate/gate.sh` / `gate/gate.ps1`; "source constraint" means the rule is an obligation on the code that no
tool checks (the rules forbid building one), so review is what enforces it.

## Architecture

| Layer | Directory | Allowed to |
| --- | --- | --- |
| infrastructure | `src/infrastructure` | define generic types and the only metaprogramming: results, strong types, bounded containers, checked arithmetic, contracts, tracing |
| interior | `src/interior` | compute: options, monitor geometry, the session plan, the per-frame step plan, NGX parameter lists. No OS or vendor header, no exception, no effect |
| simulated effects | `src/effects/sim` | interpret step plans against a seeded model of the display, capture, GPU and NGX, injecting failures |
| real effects | `src/effects/real` | talk to Direct3D 12, DirectComposition, Windows Graphics Capture, NGX, the optical flow engine, the clock and the console |
| app | `src/app` | the generic session loop (`session.h`) and the composition root (`main.cpp`) |
| tests | `tests` | property tests and the seed fuzzer, portable |
| gate | `gate` | mutation testing, the dependency lock and the gate scripts |

The interior never sees an effect: it turns a `FrameState` and a `FrameInput` into a `FramePlan`, a
bounded list of steps (`Transition`, `Dispatch`, `CopyBuffer`, `ClearTarget`, `EvaluateSr`,
`EvaluateNr`, `Draw`, `Submit`, `Present`) and the next state. `app::RunSession` is generic over the
environment; `real::RealEnvironment` records the steps into Direct3D 12 command lists and
`sim::SimEnvironment` checks them against a resource-state model. The same loop runs both.

## Scope

Under amendment A2 the rules bind shipped product code: `src/infrastructure`, `src/interior`,
`src/effects/real` and `src/app`. `tests` and the simulator in `src/effects/sim` (a test double that only
`tests/simulation_test.cpp` uses; the executable never links it) are exempt, as are the gate scripts, and
are written the ordinary way. What still binds them is listed in A2: dependency provenance, no fabricated
pass, independent expected results, no invented contracts, and no silent no-op double.

## Rule by rule

| Rule | How the code meets it | Enforced by |
| --- | --- | --- |
| R1 One operation per function | Functions have at most five statements and one branch; every behaviour-changing condition is a named predicate; compositions are `and_then`/`transform` chains of named calls. A `switch` or `std::visit` over a closed set counts as one match. | Source constraint |
| R2 Single assignment, no mutation | Bindings are `const`; state is rebuilt with `WithX` constructors (`infra::WithElement`, `BoundedVector::Push`); iteration is `std::ranges::fold_left`, `infra::FoldResult`, `infra::ForEach`, `std::views::iota` and `infra::Generated`. The waivered exceptions are listed below. | `const` bindings; the rest is a source constraint |
| R3 The call graph is static | No recursion, no virtual dispatch in domain code, no reflection or code loading. OS callbacks (`WindowProc`, `EnumDisplayMonitors`) and the NVIDIA function tables are external and stay in `src/effects/real`. `nvofapi64.dll` is loaded by name from the driver because NVIDIA ships no import library; the entry point is bound once at start-up. | Source constraint; COM and vendor interfaces are the effect boundary |
| R4 Names are specifications | A changed behaviour is a new function with a new name and every caller repointed; there is no `V2`. | Review; the commit history of this branch |
| R5 No pass-through functions | A function forwarding its arguments unchanged to one call does not exist; constructors that bind a constant or narrow a type do. | Source constraint |
| R6 No unreferenced code | Every function is reachable from `main`, the OS callbacks or the test suites. | Compiler (`-Wunused-function`, MSVC C4505); whole-program reachability is a source constraint |
| R7 No duplicated logic | Bodies are compared token by token with called names normalised; one rule, one function. | Source constraint (one waiver: the two interpreters of the step variant) |
| R8 No primitive types in interior signatures | Untrusted input becomes a domain type once, at the edge, through `Parse` functions that are the only constructors of `infra::Strong` types (`PixelCount`, `Coordinate`, `Fraction`, `Scale`, `LevelIndex`, ...). Interior functions never re-check. | Private constructors and `friend Tag` in `strong.h`; no type punning outside the effect layer is a source constraint |
| R9 Distinct meanings get distinct types | `FrameNumber`, `FenceValue`, `Instant`, `Microseconds`, `ByteCount`, `PixelCount`, `Coordinate`, `MonitorIndex`, `RequestedMonitor`, `SetIndex`, `LevelIndex`, `FrameSlot`, `BackBufferIndex` are separate tags; a transposition does not compile. | Compiler |
| R10 Effects are declared in the type | C++ has no effect system, so the module boundary stands in: interior and infrastructure code cannot include an OS, vendor or effect header; every effectful function lives in `src/effects/real`, returns a `Result`, and receives its device, window and console explicitly. | Source constraint: the module boundary |
| R11 Dependencies arrive through a declared channel | The composition root builds the `Options`, `Geometry`, `SessionPlan`, `GpuDevice`, `NgxRuntime`, `Gpu` and `Console` once and passes them by value or const reference; no globals, singletons or ambient state. | Source constraint; one waiver: the trace ring |
| R12 Exhaustive matching | Every `switch` over an internal enum lists every enumerator without `default`; `std::visit` over the step variant handles every alternative. | Compiler (`-Wswitch-enum`, MSVC C4062 as errors); the wildcard prohibition is a source constraint; one waiver: the OS message switch |
| R13 Checked arithmetic | Unbounded values use `infra::CheckedAdd/Sub/Mul/Div` (compiler builtins or `intsafe.h`); everything else is a bounded type whose range is stated by its parser (`kMaxPixelCount`, `kMaxLevels`, `kMaxMonitors`, `kFrameLimit`). | Compiler (`-Wconversion`, `-Wsign-conversion`); property tests of the checked operations |
| R14 Errors are values | Every fallible function returns `std::expected` (`infra::Result`, `infra::Status`) with an enumerated error; there is no `throw`, `try`, `catch`, `new` or throwing accessor in the code. | `noexcept` on every function; the absence of `throw`, `try` and `catch` is a source constraint |
| R15 Two outcomes, no fallbacks | A failure stops the program with its `ApiCall` and code. Nothing substitutes for a failed operation: a missing optical-flow build, an unavailable NGX feature or an unsupported adapter is an error, not a passthrough. Running without a model is only possible through explicit options (`--nr off`, `--sr off`, `--mv none`), which the plan records and the log states. | Review; the simulator's failure injection checks that every failure variant propagates |
| R16 Invariants are contracts, and contracts run in production | `REQUIRE`/`ENSURE` are compiled in every configuration; a violation prints the predicate and location, dumps the trace ring to `dlssscreen-trace.txt` and aborts. | `infrastructure/contracts.h`, `trace.cpp` |
| R17 Every result is handled | Every non-void function is `[[nodiscard]]`; `std::expected` results are consumed by `and_then`, `transform`, `value_or` or `error_or`. | Compiler (`-Wunused-result`, MSVC C4834) on the `[[nodiscard]]` every function carries |
| R18 Writers verify their output | `CreateTexture` re-reads the resource description; `WriteZeros` reads the mapped bytes back; every NGX parameter write is read back (`NgxParameterRoundTrip`); `Log` checks the byte count of every write and the flush. | Code; `[[nodiscard]]` on the verification result makes it impossible to drop |
| R19 Nothing unfinished compiles | No TODO, FIXME, placeholder or stub on a shipped path. | Source constraint |
| R20 Bound every wait | Fence waits time out after 4 s, the frame-latency wait after 2 s; the capture drain reads at most 8 frames, the message pump 64 messages, adapter enumeration 16 adapters, monitor enumeration 16 monitors, the session 2^40 frames. | Constants next to each wait |
| R21 Bound every resource | `infra::BoundedVector` and `infra::BoundedString` reject on overflow (`CapacityExceeded`, `StringTooLong`); the step list holds 192 steps, the parameter list 48, the argument list 64, the trace ring 65536 entries (oldest evicted). | Source constraint: the capacity constants next to each bounded type |
| R22 No shared mutable state across threads | The process is single-threaded: the free-threaded capture pool is polled, never delivered by callback; the Direct3D 11 context is protected only because the free-threaded frame pool requires it. There is no lock in the code. | Review; the thread sanitizer has nothing to observe because no thread is created |
| R23 All non-determinism is an input | The clock enters as `FrameInput.now`, capture arrival as `freshCapture`, hotkeys as toggles; the simulator draws all of them from a seeded SplitMix64 generator. Production code draws no randomness. | `interior/frame.h`, `effects/sim` |
| R24 No direct contact with reality | Only `src/effects/real` reads the clock, the GPU, the capture pool or the console. The simulator runs the same session loop from a numeric seed and injects device loss, capture loss, fence timeouts, NGX failures, scene cuts and clock jumps between frames; every failure reproduces from its seed. The tool is long-running and writes no persistent state except the optional append-only log mirror, so kill-at-random-point leaves nothing to recover. | `tests/simulation_test.cpp`; ctest seeds 1000, 2000, 3000; gate seeds 4000, 5000, 6000 |
| R25 Every call is traced | The compiler instruments every function outside `trace.cpp` (`-finstrument-functions`, or `/Gh /GH` with `trace_msvc.asm`); the ring records entry and exit addresses and is dumped on a contract violation. C++ has no reflection, so argument values are not serialised; the replay of a run is the seed under the simulator. | `CMakeLists.txt` (`DSCREEN_TRACE`), `trace.cpp` |
| R26 Tests are properties, and tests are tested | Fifty-one properties over generated inputs, including every failure variant of the parsers and planners; the option parser is fuzzed with random argument vectors; the simulator seed fuzzer runs whole sessions. `gate/mutate.py` introduces defects (`<`/`<=`, `==`/`!=`, `+`/`-`, `true`/`false`, `&&`/`||`) and fails the gate when the tests keep passing. Last sampled run: 60 of 135 mutants, 52 killed, 8 survived, score 0.87 (test seed 1000, sample seed 1). | ctest; gate (mutation threshold 0.8) |
| R27 No speculative generality | The environment abstraction has two implementations (real and simulated); every option is consumed by the plan; the single feature flag is inventoried. | Review |
| R28 Comments are the last resort | Comments are waivers, citations, rationale or invariants; no doc comments, restated code or commented-out code. | Source constraint |
| R29 Standard library first | `std::expected`, ranges, `std::format`, `std::to_chars`; the only external code is NVIDIA's (DLSS SDK, optical flow SDK) and the platform. | `gate/dependencies.lock`, `gate/check_lock.py` |
| R30 Consult the index before writing | Ordinary repository search by name and type before a function is written. | Workflow constraint |
| R31 Metaprogramming is confined to infrastructure | Templates and macros live in `src/infrastructure` (results, strong types, bounded containers, folds, formatting, contracts, tracing). The waivered exceptions are the effect interfaces (`com.h`, `session.h`), the composition root and the generic option-table lookups. Shader byte code is a build artefact produced by DXC from the checked-in HLSL, like an object file. `DSCREEN_HAVE_NVOF` is the only feature flag; the gate builds both values. | Source constraint |
| R32 A change does what it says | Commits on this branch each state one change. | Review |
| A1 Dependency provenance | The only non-standard code is NVIDIA's (the DLSS SDK, the optical flow SDK) and the platform's own (Direct3D, DirectComposition, Windows Graphics Capture, WinTrust); there is no package manager and no other package. | `gate/dependencies.lock`, `gate/check_lock.py` |
| R33 Every option is decided at its owning boundary | Records the OS or a library reads name every field (designated initialisers, or zero-initialise and assign by name where a union forbids them); no default arguments; every `switch` and `std::visit` names every alternative. | Compiler (initialisation and exhaustiveness diagnostics); the rest is a source constraint, see `CLAUDE.md` |
| R34 Nested functions are functions | A helper that serves one function is defined inside it as a named, captureless lambda held in a `static constexpr` local, to any depth; a helper that serves several functions is defined in the lowest function enclosing all of them, and one that serves several top-level functions stays at namespace scope. Static storage is what lets a sibling call a sibling without a capture, so no nested lambda can reach an enclosing local at all, and every dependency is a parameter or a namespace-scope name. Bodies are unchanged; a helper whose address the OS takes (a window procedure, an enumeration callback) stays a function. | Compiler: a `[]` lambda that names an enclosing local does not compile. Placement is a source constraint |
| R35 Detect completion; do not guess it | Fence waits use the event the fence signals; the frame-latency wait uses the swap chain's waitable object; the capture pool is drained with `TryGetNextFrame` from the session loop. | Source constraint; the capture drain has not been assessed against R35's polling waiver and is an open item |
| R36 Request inaccessible documentation | The window-exclusion interface of Windows Graphics Capture has no published documentation; its layout was taken from the metadata-generated bindings and its behaviour established by observation (`exclusion.cpp`). | Workflow constraint |

## Interpretations

- A `switch` or `std::visit` over a closed set is one match, not one decision per arm.
- A chain of `and_then`/`transform` calls is one composition statement.
- Templates are the generic types the rules require (strong types, results, bounded containers) and are
  confined to infrastructure; the effect interfaces that must be generic over the two environments carry waivers.
- COM and NVIDIA vtable calls are external effects; they are the boundary R10 draws.
- A function returning `void` is an effect and appears only in the effect layer or as an infrastructure sink.

## Waivers

Every waiver carries its rule number and reason next to the line (`git grep WAIVER` lists them).
They fall into these groups:

- R2 (state replaced whole): the simulator world, the real environment's frame context and counters,
  the trace ring, the monitor-enumeration callback's output slot, the session loop's frame state.
- R2 (loops): the session loop, the trace dump during a panic, the GPU search window in `Match.hlsl`,
  the test drivers and generators.
- R1: the session loop, whose body is the bounded iteration itself.
- R7: the real and the simulated interpreter of the step variant share the dispatch shape.
- R11: the trace ring written by compiler-inserted hooks that carry no context.
- R12: the window-message switch, an open set defined by the OS.
- R31: `com.h` and `session.h` (effect interfaces), `main.cpp` (composition root), the option-table lookups.

## The gate

`gate/gate.sh` (portable targets) and `gate/gate.ps1` (everything, including `DlssScreen.exe`) run:

1. the build with warnings as errors and tracing on (IX.1, IX.3);
2. the formatter check (`.clang-format`, IX.5);
3. the property tests under the ctest seeds and three further seeds;
4. the address and undefined-behaviour sanitizers (portable targets);
5. mutation testing with the threshold; the surviving mutants of the last run are the open test gaps:
   `monitors.cpp` ordering and union arithmetic, `options.cpp` value validation, `plan.cpp` cursor and quality
   choice, `frame.cpp` finest-level flags and history staleness;
6. the dependency-lock check against the installed SDK headers.

Steps 3 to 6 are verification the rules leave optional (IX); they are run here because they exist, and
the routine gate is steps 1 and 2. There is no rules lint: the rules forbid building one, and what a tool
cannot check is a source constraint enforced by review.

## Differences from the first implementation

- No fallbacks: the first version silently ran as a passthrough when NGX was unavailable and fell back
  from optical flow to block matching; both are errors now (R15).
- NGX log lines go to NGX's own sinks; the log callback needed a global to reach the console (R11).
- The output is a `Result` at every step, so a failing call names the API and its code instead of an
  exception message.
- A contract violation aborts with the trace ring written next to the working directory (R16, R25).
