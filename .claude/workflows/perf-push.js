export const meta = {
  name: 'perf-push',
  description: 'Diagnose why the native quad-cover solve is 7-13x slower than QuadriFlow, adversarially cross-examine each optimization, and synthesize a ranked plan — quality (output) must not change',
  whenToUse: 'Run to attack solve performance: grounded in CYBER_QC_TIME profiling, get back a verified, ranked set of concrete speedups that preserve the output',
  phases: [
    { title: 'Diagnose', detail: 'one expert lens per hot spot, read-only' },
    { title: 'Cross-examine', detail: 'adversarial skeptic per optimization (speedup real? output unchanged?)' },
    { title: 'Synthesize', detail: 'merge survivors into a ranked, sequenced plan' },
  ],
}

const IN = args || {}
const PROFILE = IN.profile ||
  `MEASURED (CYBER_QC_TIME + CYBER_QC_DEBUG, target 3000 quads, cheburashka ~15s wall):
  - ours 8-15s vs QuadriFlow ~1s => 7.5-13x SLOWER (spot 7.5x, fandisk 10.8x, rocker 8.6x, cheb 12.9x, bunny 7.9x).
  - Per remesh the SOLVE is 99% of time: [qc-time] isotropic=67ms field+setup=40ms solve=7090ms.
  - The QuadCoverQuadrangulator CALIBRATION LOOP runs the whole ~7s solve TWICE (7090ms + 6987ms ~= 14s) to hit the quad count.
  - Inside the solve, the INTEGER ROUNDING dominates: [qc] reduced nCut=894 seams=151 vars=2090 free=1458 intFree=220 maskedSolves=43 totalCg=4878.
    i.e. 43 masked-CG re-solves (greedy round-one-then-re-solve), ~4878 total CG iterations, on a FIXED reduced operator.`

const CTX = `GOAL: make the native quad-cover solve dramatically faster (target: close the 7-13x gap to QuadriFlow) WITHOUT changing the output mesh (or with an explicit, measured, opt-in quality tradeoff only). Speed is the axis with the most headroom and least risk — the output is deterministic, so a correct optimization is byte-identical or provably equivalent.

${PROFILE}

KEY SOURCE (native dependency-free quad-cover path):
  src/quadrangulate/src/quadcover_extractor.cpp
    - QuadCoverQuadrangulator::quadrangulate (~line 2214): the 2-attempt calibration loop
      'for (int attempt = 0; attempt < 2; ++attempt)' — computeSeamlessUv, extract, if the count
      ratio is off, rescale and RE-SOLVE THE WHOLE THING. This is the 2x double-solve.
    - computeSeamlessUvNative (~line 176, CYBER_QC_TIME timers): isotropic remesh -> buildSeamlessSetup -> solveParameterization.
  src/quadrangulate/src/seamless_solver.cpp
    - solveParameterization (~line 850): cotangent Poisson (CG on accel::spmv) + ARAP polish (kArapIters=6, each RE-SOLVES the Poisson) + the integer-seamless MIQ.
    - solveSeamlessReduced (~line 448): the sparse constraint-elimination MIQ. Builds a FIXED reduced
      Dirichlet operator (T^T blkdiag(L,L) T), then GREEDY BATCHED ROUNDING (lines ~755-828): relaxed
      solve, pin a batch (frac.size()/8) of closest-to-integer frees, RE-SOLVE, repeat.
    - maskedSolve (~line 668): each rounding round's masked CG on the pinned operator (totalCg accumulates); warm-started from the incoming value but still ~100 iters each.
  Available: the vendored SciPP (scipp::sparse) provides a sparse direct solver (spsolve = Cholesky/LU)
    and CG/GMRES with preconditioning — behind CYBER_WITH_SCIPP (currently only linked for the MCF experiment). A ONE-TIME factorization of the fixed operator + cheap back-substitution per round could replace the 43 CGs.

HARD CONSTRAINT: the optimization must PRESERVE THE OUTPUT (deterministic, byte-identical) OR be an explicit opt-in with a measured quality delta. An optimization that silently changes the mesh is a REGRESSION, not a speedup. State the output-equivalence argument for every proposal.

HOW TO MEASURE (read-only; the build already exists, do NOT rebuild):
  CYBER_QC_TIME=1 CYBER_QC_DEBUG=1 examples/run.sh <a script that remeshes with quad_method='quad-cover'>
  -> prints [qc-time] isotropic/field+setup/solve ms and [qc] reduced maskedSolves/totalCg per solve.`

const DIAGNOSE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['lens', 'hotSpot', 'optimization', 'expectedSpeedup', 'outputEquivalence', 'effort', 'confidence', 'evidence', 'risks'],
  properties: {
    lens: { type: 'string' },
    hotSpot: { type: 'string', description: 'the specific code + why it costs time, tied to the profile numbers' },
    optimization: {
      type: 'object', additionalProperties: false, required: ['summary', 'files', 'codeSketch'],
      properties: {
        summary: { type: 'string' },
        files: { type: 'array', items: { type: 'string' }, description: 'exact file:function to edit' },
        codeSketch: { type: 'string', description: 'concrete change referencing real symbols' },
      },
    },
    expectedSpeedup: { type: 'string', description: 'estimated wall-clock reduction, e.g. "~2x (removes the double-solve)" or "solve 7s->2s"' },
    outputEquivalence: { type: 'string', description: 'why the output mesh is unchanged (byte-identical) — or the explicit quality tradeoff if not' },
    effort: { type: 'string', enum: ['S', 'M', 'L'] },
    confidence: { type: 'number' },
    evidence: { type: 'array', items: { type: 'string' } },
    risks: { type: 'array', items: { type: 'string' } },
  },
}

const VERDICT_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['lens', 'survives', 'speedupRealistic', 'preservesOutput', 'revisedConfidence', 'refutation', 'regressionRisk'],
  properties: {
    lens: { type: 'string' },
    survives: { type: 'boolean' },
    speedupRealistic: { type: 'boolean', description: 'did you verify in the source that this actually sits on the hot path and the speedup is plausible' },
    preservesOutput: { type: 'boolean', description: 'does it truly leave the output unchanged (or is the tradeoff acceptable + measured)' },
    revisedConfidence: { type: 'number' },
    refutation: { type: 'string', description: 'strongest argument the speedup is smaller than claimed OR it changes the output' },
    regressionRisk: { type: 'string', enum: ['low', 'medium', 'high'] },
  },
}

const PLAN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['topStep', 'rankedPlan', 'combinedExpectedSpeedup', 'conflicts', 'sequencing'],
  properties: {
    topStep: {
      type: 'object', additionalProperties: false, required: ['lens', 'summary', 'files', 'firstEdit', 'whyFirst', 'howToMeasure'],
      properties: {
        lens: { type: 'string' }, summary: { type: 'string' },
        files: { type: 'array', items: { type: 'string' } },
        firstEdit: { type: 'string' }, whyFirst: { type: 'string', description: 'highest speedup x confidence / effort at lowest output-change risk' },
        howToMeasure: { type: 'string', description: 'exact CYBER_QC_TIME command + the ms/maskedSolves delta that confirms it, AND how to confirm the output is unchanged' },
      },
    },
    rankedPlan: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false, required: ['rank', 'lens', 'optimization', 'confidence', 'expectedSpeedup', 'effort', 'preservesOutput'],
        properties: {
          rank: { type: 'number' }, lens: { type: 'string' }, optimization: { type: 'string' },
          confidence: { type: 'number' }, expectedSpeedup: { type: 'string' }, effort: { type: 'string' },
          preservesOutput: { type: 'boolean' },
        },
      },
    },
    combinedExpectedSpeedup: { type: 'string' },
    conflicts: { type: 'array', items: { type: 'string' } },
    sequencing: { type: 'string' },
  },
}

const LENSES = [
  {
    key: 'calibration-double-solve',
    prompt: `LENS: the 2x calibration double-solve. QuadCoverQuadrangulator::quadrangulate (quadcover_extractor.cpp ~2214)
runs the entire ~7s solve, extracts, and if the quad count is off re-solves from scratch at a corrected
scaling. That is the single biggest slice (~2x). Can we hit the target count in ONE solve — predict the
scaling analytically from mesh area / target (meshTargetQuads already exists) so the first solve lands in
tolerance, or extrapolate from a cheap proxy — so the second solve is rare/never? Does the output change
(the accepted solve is whichever lands in tolerance; a better first guess that still lands in tolerance is
output-equivalent for that attempt)? Read the loop + meshTargetQuads. Propose a concrete change.`,
  },
  {
    key: 'factorization-reuse',
    prompt: `LENS: replace the 43 masked CG solves with ONE factorization + back-substitution. In solveSeamlessReduced
(seamless_solver.cpp ~448), the reduced Dirichlet operator T^T blkdiag(L,L) T is FIXED across all rounding
rounds — only the RHS / the pinned mask changes. Yet maskedSolve (~668) runs a fresh masked CG each round
(43 rounds, ~4878 CG iters). A sparse direct factorization (Cholesky, since SPD) computed ONCE, then each
round is a cheap triangular solve — potentially the biggest single win. The vendored SciPP (scipp::sparse
spsolve / Cholesky, behind CYBER_WITH_SCIPP) could provide it. Investigate: is the masked/pinned operator
still factorable per round (the mask changes which rows are pinned), or is there a Schur-complement / low-rank
update trick? Output-equivalent by construction (same linear system, exact solve vs CG-to-tolerance).`,
  },
  {
    key: 'masked-cg-warmstart-precond',
    prompt: `LENS: cut the ~4878 CG iterations. maskedSolve (seamless_solver.cpp ~668) runs a masked CG per rounding
round. Each round only pins ONE MORE batch of integers (a small perturbation), so warm-starting from the
previous round's solution should converge in far fewer iters — is it fully warm-started? Add a preconditioner
(Jacobi/diagonal at minimum, or incomplete Cholesky) to the CG. Is the tolerance (rsNew<=1e-12*rs0) tighter
than the rounding needs? Read maskedSolve + the rounding loop. Propose concrete warm-start/precond/tolerance
changes and argue output-equivalence (a preconditioner changes iterations, not the converged solution).`,
  },
  {
    key: 'rounding-batch-schedule',
    prompt: `LENS: fewer rounding rounds. The greedy batched rounding (seamless_solver.cpp ~755-828) pins a batch of
frac.size()/8 closest-to-integer frees per round, then re-solves — 43 rounds here. A larger batch (fewer
rounds) means fewer re-solves. But the reduced integers are UNCONSTRAINED (the comment says rounding them to
ANY integers stays seamless) — so how much re-solving is actually needed for QUALITY vs just pinning all at
once? Read the loop + the 'reduced integers are unconstrained' argument. Propose a batch schedule (or a
one-shot round) and HONESTLY assess the output/quality delta — this one likely DOES change the mesh, so
quantify the tradeoff and whether it should be opt-in.`,
  },
  {
    key: 'arap-poisson-resolves',
    prompt: `LENS: the ARAP polish re-solves. solveParameterization runs kArapIters=6 ARAP rounds, each re-assembling
the RHS and RE-SOLVING the Poisson (relaxedSolve). That's ~6 extra full solves before the integer phase.
Are they warm-started (the field only rotates a little each round)? Could the same one-time factorization
(see factorization-reuse) serve these too? Could the ARAP converge in fewer iters, or share the operator with
the integer phase? Read the ARAP loop + relaxedSolve. Propose a concrete change and argue output-equivalence
(warm-start / shared factorization = same result, fewer flops).`,
  },
  {
    key: 'accel-parallelism',
    prompt: `LENS: is the hot path using the hardware? The CG runs on accel::spmv (the compute-accel layer, which can
dispatch to GPU). Is the native solve actually running spmv on a fast backend, or the CPU reference single-
threaded? Is the cotan Laplacian assembly / the 43-round loop parallelisable? Would a GPU backend (or a
multi-threaded CPU spmv, or SIMD) cut the solve? Read how solveParameterization/maskedSolve dispatch through
accel and what backend the pipeline uses by default. Propose the most impactful, lowest-risk parallelism
change (output-equivalent: same math, more lanes).`,
  },
]

phase('Diagnose')
log(`Perf push on the native quad-cover solve (7-13x slower than QuadriFlow): ${LENSES.length} lenses -> adversarial cross-examine -> ranked plan`)

const results = await pipeline(
  LENSES,
  (l) =>
    agent(
      `${CTX}\n\n====\nYou are a numerical-performance / sparse-linear-algebra expert. ${l.prompt}\n\n` +
        `Read the cited source before claiming anything. You MAY run the read-only CYBER_QC_TIME profiling but must NOT rebuild or edit code. ` +
        `Return a rigorous, code-grounded diagnosis and a concrete optimization. For EVERY proposal, give the output-equivalence argument (byte-identical, or an explicit measured tradeoff). Be honest about the speedup magnitude and confidence.`,
      { label: `diag:${l.key}`, phase: 'Diagnose', schema: DIAGNOSE_SCHEMA },
    ),
  (diag, l) => {
    if (!diag) return null
    return agent(
      `${CTX}\n\n====\nYou are a SKEPTIC. A colleague proposed this speedup for the ${l.key} lens. Try hard to REFUTE it on TWO fronts: ` +
        `(1) is the speedup real and on the hot path (open the cited files and verify it actually sits where the profile says the time goes), and ` +
        `(2) does it silently CHANGE THE OUTPUT MESH (a speedup that alters the result is a regression). Default to survives=false if the code path does not support the claim, the speedup is speculative, or output-equivalence is not sound.\n\n` +
        `PROPOSED OPTIMIZATION (JSON):\n${JSON.stringify(diag, null, 2)}`,
      { label: `verify:${l.key}`, phase: 'Cross-examine', schema: VERDICT_SCHEMA },
    ).then((v) => ({ lens: l.key, diag, verdict: v }))
  },
)

const clean = results.filter(Boolean).filter((r) => r.verdict)
const survivors = clean.filter((r) => r.verdict.survives)
log(`${clean.length}/${LENSES.length} lenses diagnosed; ${survivors.length} survived cross-examination`)

phase('Synthesize')
const plan = await agent(
  `${CTX}\n\n====\nYou are the tech lead. Below are ${clean.length} diagnosed optimizations, each with an adversarial verdict. ` +
    `Synthesize a single ranked, sequenced plan to close the 7-13x gap to QuadriFlow. Weight by (expected speedup x revised confidence / effort) and HARD-PENALIZE anything that changes the output (prefer byte-identical wins; a quality tradeoff must be opt-in and measured). Identify conflicts / overlaps (e.g. a shared one-time factorization serving both ARAP and the integer rounding). Pick ONE topStep: the highest-ROI, lowest-output-risk first change, with the exact first edit, and the exact CYBER_QC_TIME command + ms/maskedSolves delta that confirms the speedup AND confirms the output is unchanged.\n\n` +
    `DIAGNOSED OPTIMIZATIONS WITH VERDICTS (JSON):\n${JSON.stringify(clean, null, 2)}`,
  { label: 'synthesize', phase: 'Synthesize', schema: PLAN_SCHEMA },
)

return { target: 'native quad-cover solve speed', survivors: survivors.map((s) => s.lens), plan, allLenses: clean }
