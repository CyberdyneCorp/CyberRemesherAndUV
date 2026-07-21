export const meta = {
  name: 'quad-quality-push',
  description: 'Diagnose why our quad topology trails the references on a target model, adversarially cross-examine each fix, and synthesize a ranked implementation plan',
  whenToUse: 'Run to keep pushing quad quality: pick a failing case (e.g. the Stanford-bunny ears), pass baseline numbers via args, get back a verified, ranked set of concrete code fixes',
  phases: [
    { title: 'Diagnose', detail: 'one expert lens per candidate root cause, read-only' },
    { title: 'Cross-examine', detail: 'adversarial skeptic per proposed fix' },
    { title: 'Synthesize', detail: 'merge survivors into a ranked, sequenced plan' },
  ],
}

// ---- Inputs (args) with defaults for the flagship bunny-ears case ------------
const IN = args || {}
const MODEL = IN.model || 'stanford-bunny'
const FINDING = IN.earFinding ||
  'In the upper/ear region the native quad-cover emits 38 irregular (valence!=4) interior vertices vs QuadriFlow 8 and AutoRemesher 26 — ~5x the singularities, clustered on the thin high-curvature ear tubes, which twists the edge flow.'
const BASELINE = IN.baseline || {
  quadcover: { irr: 5, med: 80, cv: 0.33, dev: 0.39, nerr: 12.8, quads: 3384 },
  quadriflow: { irr: 4, med: 82, cv: 0.17, dev: 0.32, nerr: 10.7, quads: 2695 },
  autoremesher: { irr: 12, med: 75, cv: 0.45, dev: 0.32, nerr: 10.0, quads: 3446 },
}
const PRIOR = IN.priorFindings || [
  'PROVEN DEAD END — do not re-propose: multi-resolution COARSE extraction (a no-op for quality).',
  'ALREADY SHIPPED: a valence-3/5 adjacent-dipole canceller in quad_extract.cpp (cut irr ~27%); propose extensions, not a re-implementation.',
  'Known limitation: sharp-edge handling degrades at low target density.',
  'ARAP distortion polish + non-quad-cap elimination already ship in seamless_solver.cpp / quad_extract.cpp.',
  'MEASURED & REFUTED (do not re-propose for the bunny): raising CYBER_QC_FEATURE_DEG (noisy, regresses CAD); curvature-weighted cut-graph seam routing (net-negative corpus-wide, reverted).',
  'ALREADY SHIPPED (M8, experimental): multiresolution cross field (computeCrossFieldFromOrientation, gate CYBER_QC_CROSSFIELD_MULTIRES) helps smooth models; tube-aware coarsening (coarsen in position_field.cpp, CYBER_QC_COARSEN_MINDOT) stops the hierarchy bridging thin tubes. Propose extensions, not re-implementations.',
  'CRITICAL MEASUREMENT CAVEAT: the quad-count calibration makes ear/irregular counts swing ~±10 between runs. Judge levers on the FULL-CORPUS benchmark, never single-model sweeps. The current top blocker for the bunny is extraction NON-DETERMINISM, not the field algorithm.',
]

const CTX = `TARGET MODEL: ${MODEL}
FLAGSHIP DEFECT: ${FINDING}

BASELINE (matched count, from examples/11_benchmark.py):
  ours quad-cover (native): irr ${BASELINE.quadcover.irr}%  median ${BASELINE.quadcover.med}deg  edgeCV ${BASELINE.quadcover.cv}  surfDev ${BASELINE.quadcover.dev}%  normalErr ${BASELINE.quadcover.nerr}deg
  QuadriFlow (target):      irr ${BASELINE.quadriflow.irr}%  median ${BASELINE.quadriflow.med}deg  edgeCV ${BASELINE.quadriflow.cv}  surfDev ${BASELINE.quadriflow.dev}%  normalErr ${BASELINE.quadriflow.nerr}deg
  AutoRemesher:             irr ${BASELINE.autoremesher.irr}%  median ${BASELINE.autoremesher.med}deg  edgeCV ${BASELINE.autoremesher.cv}

WHY OVERALL irr% LOOKS CLOSE BUT THE EARS LOOK BAD: the ear is a small fraction of surface area, so 38 clustered
singularities only lifts overall irr from 4->5%, but locally it destroys edge flow. The defect surfaces in the
GLOBAL metrics as edge CV 0.33 vs 0.17 (our quads are 2x less uniform) and normal err 12.8 vs 10.7 (our quads
hug the curved ear worse). Drive on: (1) ear-region irregular count, (2) edge CV, (3) normal err, (4) overall irr%.

KEY SOURCE FILES (native dependency-free QuadCover path):
  src/quadrangulate/src/crossfield.cpp        - cross-field solve; CYBER_QC_FIELD_DAMP self-damping (default 0.15; smaller=smoother=fewer singularities); curvature alignment
  src/quadrangulate/src/position_field.cpp    - smoothOrientation/smoothPosition, multiresolution hierarchy that PLACES singularities
  src/quadrangulate/src/seamless_solver.cpp   - solveParameterization: cut graph (Poincare-Hopf), cotangent Poisson (CG), sparse-elim MIQ integer rounding, ARAP polish; kFieldIterations=40
  src/quadrangulate/src/quadcover_extractor.cpp - computeSeamlessUvNative: isotropicRemesh + feature tagging + setup + solve + assemble
  src/quadrangulate/src/quad_extract.cpp      - isoline/graph extraction, graph cleanup, dipole/valence cleanup, irregular detection

DIAGNOSTICS YOU MAY RUN (read-only; the build already exists, do NOT rebuild):
  examples/run.sh examples/scratch_ears.py                        -> renders the ear region + prints "N irregular verts in ear" per engine
  examples/run.sh examples/11_benchmark.py --models ${MODEL}      -> irr% / edge CV / normal err per method

PRIOR FINDINGS — respect these, do not repeat dead ends:
${PRIOR.map((p) => '  - ' + p).join('\n')}`

const DIAGNOSE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['lens', 'rootCause', 'mechanism', 'fix', 'expectedImpact', 'effort', 'confidence', 'evidence', 'risks'],
  properties: {
    lens: { type: 'string' },
    rootCause: { type: 'string', description: 'the specific reason THIS lens produces extra ear singularities, tied to real code' },
    mechanism: { type: 'string', description: 'why the fix reduces singularities / improves flow on thin high-curvature tubes' },
    fix: {
      type: 'object', additionalProperties: false, required: ['summary', 'files', 'codeSketch'],
      properties: {
        summary: { type: 'string' },
        files: { type: 'array', items: { type: 'string' }, description: 'exact file:function to edit' },
        codeSketch: { type: 'string', description: 'concrete change: constants, added pass, or algorithm, referencing real symbols' },
      },
    },
    expectedImpact: {
      type: 'object', additionalProperties: false, required: ['earIrr', 'edgeCv', 'normalErr', 'overallIrr', 'rationale'],
      properties: {
        earIrr: { type: 'string' }, edgeCv: { type: 'string' }, normalErr: { type: 'string' },
        overallIrr: { type: 'string' }, rationale: { type: 'string' },
      },
    },
    effort: { type: 'string', enum: ['S', 'M', 'L'] },
    confidence: { type: 'number', description: '0..1 that this fix is real and helps without regressing other models' },
    evidence: { type: 'array', items: { type: 'string' }, description: 'file:line citations or diagnostic output supporting the diagnosis' },
    risks: { type: 'array', items: { type: 'string' } },
  },
}

const VERDICT_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['lens', 'survives', 'codePathSupportsIt', 'revisedConfidence', 'refutation', 'sideEffects', 'regressionRisk'],
  properties: {
    lens: { type: 'string' },
    survives: { type: 'boolean' },
    codePathSupportsIt: { type: 'boolean', description: 'did you verify in the source that the proposed edit point exists and the mechanism is wired the way the diagnosis claims' },
    revisedConfidence: { type: 'number' },
    refutation: { type: 'string', description: 'strongest argument the fix will NOT reduce ear singularities or will regress CV/normalErr/other models' },
    sideEffects: { type: 'array', items: { type: 'string' } },
    regressionRisk: { type: 'string', enum: ['low', 'medium', 'high'] },
  },
}

const PLAN_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['topStep', 'rankedPlan', 'combinedExpectedImpact', 'conflicts', 'sequencing'],
  properties: {
    topStep: {
      type: 'object', additionalProperties: false, required: ['lens', 'summary', 'files', 'firstEdit', 'whyFirst', 'howToMeasure'],
      properties: {
        lens: { type: 'string' }, summary: { type: 'string' },
        files: { type: 'array', items: { type: 'string' } },
        firstEdit: { type: 'string', description: 'the exact first code change to make' },
        whyFirst: { type: 'string', description: 'highest ROI: impact x confidence / effort, and lowest regression risk' },
        howToMeasure: { type: 'string', description: 'exact command + metric delta that confirms success' },
      },
    },
    rankedPlan: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false, required: ['rank', 'lens', 'fix', 'confidence', 'expectedImpact', 'effort'],
        properties: {
          rank: { type: 'number' }, lens: { type: 'string' }, fix: { type: 'string' },
          confidence: { type: 'number' }, expectedImpact: { type: 'string' }, effort: { type: 'string' },
        },
      },
    },
    combinedExpectedImpact: { type: 'string' },
    conflicts: { type: 'array', items: { type: 'string' }, description: 'fixes that interact or must not be combined' },
    sequencing: { type: 'string', description: 'the order to implement and why' },
  },
}

// One expert lens per candidate root cause. Each is read-only and must ground
// its claims in the real source before proposing a fix.
const LENSES = [
  {
    key: 'crossfield-smoothness',
    prompt: `LENS: cross-field smoothness on thin, high-curvature tubes.
Investigate src/quadrangulate/src/crossfield.cpp. Does the cross-field self-damping (CYBER_QC_FIELD_DAMP, default 0.15)
and curvature/principal-direction alignment produce spurious index +-1/4 singularities where the ear surface bends
sharply and the tube is thin? Compare how a smoother field (heavier neighbour averaging, more iterations, or
curvature-magnitude-weighted alignment) would move/merge those cones. Read the actual smoothing loop and alignment
term. Propose a concrete change (constant, weighting, or iteration schedule).`,
  },
  {
    key: 'position-field-hierarchy',
    prompt: `LENS: multiresolution orientation/position smoothing that PLACES singularities.
Investigate src/quadrangulate/src/position_field.cpp (smoothOrientation, smoothPosition, the level hierarchy).
QuadriFlow places far fewer singularities partly via coarse-to-fine orientation smoothing. Is our coarsest level
too fine to relax the ear's cones together, or is prolongation losing the coarse decision? NOTE the prior finding
that multi-res COARSE EXTRACTION is a proven no-op — that is about extraction, NOT about field smoothing; a fix to
the smoothing schedule is still fair game. Propose a concrete change to iterations/levels/prolongation.`,
  },
  {
    key: 'cone-count-minimization',
    prompt: `LENS: singularity/cone count in the seamless solve.
Investigate src/quadrangulate/src/seamless_solver.cpp (cut graph, Poincare-Hopf index accounting, cone selection,
MIQ). QuadriFlow minimizes singularity count in its integer program; do we place every field defect as a cone with
no merging/pruning on thin tubes? Can nearby opposite-index cones on the ear be merged or a cone be relocated to a
lower-curvature spot? Read how cones/junctions are chosen and whether there is any count penalty. Propose a concrete
merge/prune/relocation step or an energy term.`,
  },
  {
    key: 'presolve-resolution',
    prompt: `LENS: pre-solve resolution / spacing on thin features.
Investigate computeSeamlessUvNative in src/quadrangulate/src/quadcover_extractor.cpp — the isotropicRemesh + spacing
that feeds the field. If the thin ear tube is under-resolved before the field solve, the field aliases and drops
extra singularities. Does uniform spacing starve the ear? Would curvature-adaptive presolve density (finer on the
ear) let the field align cleanly? The prior finding notes variable-spacing is the known unlock for adaptivity. Read
the spacing/remesh call and propose a concrete density change scoped to high-curvature regions.`,
  },
  {
    key: 'post-extraction-cleanup',
    prompt: `LENS: post-extraction singularity cleanup coverage.
Investigate the dipole/valence cleanup in src/quadrangulate/src/quad_extract.cpp. A valence-3/5 adjacent-dipole
canceller ALREADY SHIPS (do not re-implement it). Question: is it gated off in exactly the region where ear
singularities cluster (near non-quad caps / boundaries / high valence), and can its reach be extended — e.g.
valence 3-3 / 5-5 pair moves, short irregular-separation collapse, or a local re-quad of the ear patch — WITHOUT
raising overall irregular count? Read the gating conditions. Propose a concrete extension with its guard.`,
  },
  {
    key: 'miq-seam-routing',
    prompt: `LENS: MIQ integer rounding / seam routing near the ear.
Investigate solveParameterization's integer rounding and seam placement in src/quadrangulate/src/seamless_solver.cpp.
Do seams routed across the thin ear, or greedy integer rounding of the ear's parameter values, create spurious
irregular vertices at seam crossings? Would seam-aware or curvature-aware rounding (round interior-of-tube last,
keep seams off the tube crest) reduce them? Read the rounding loop and cut/seam routing. Propose a concrete change.`,
  },
]

phase('Diagnose')
log(`Quad-quality push on ${MODEL}: ${LENSES.length} diagnostic lenses -> adversarial cross-examine -> ranked plan`)

const results = await pipeline(
  LENSES,
  (l) =>
    agent(
      `${CTX}\n\n====\nYou are a quad-remeshing / field-based-parameterization expert. ${l.prompt}\n\n` +
        `Read the cited source before claiming anything. You MAY run the read-only diagnostics above but must NOT rebuild or edit code. ` +
        `Return a rigorous, code-grounded diagnosis and a concrete, minimal fix. Be honest about confidence; a weak lens should say so.`,
      { label: `diag:${l.key}`, phase: 'Diagnose', schema: DIAGNOSE_SCHEMA },
    ),
  (diag, l) => {
    if (!diag) return null
    return agent(
      `${CTX}\n\n====\nYou are a SKEPTIC. A colleague proposed this fix for the ${l.key} lens. Try hard to REFUTE it.\n\n` +
        `PROPOSED DIAGNOSIS+FIX (JSON):\n${JSON.stringify(diag, null, 2)}\n\n` +
        `Open the cited files and VERIFY the edit point exists and the mechanism is wired as claimed. Then argue the strongest case that ` +
        `this will NOT reduce ear singularities, or will regress edge CV / normal err / other models (spot, fandisk, rocker-arm, cheburashka). ` +
        `Default to survives=false if the code path does not support the claim or the mechanism is speculative. Set revisedConfidence honestly.`,
      { label: `verify:${l.key}`, phase: 'Cross-examine', schema: VERDICT_SCHEMA },
    ).then((v) => ({ lens: l.key, diag, verdict: v }))
  },
)

const clean = results.filter(Boolean).filter((r) => r.verdict)
const survivors = clean.filter((r) => r.verdict.survives)
log(`${clean.length}/${LENSES.length} lenses diagnosed; ${survivors.length} survived cross-examination`)

phase('Synthesize')
const plan = await agent(
  `${CTX}\n\n====\nYou are the tech lead. Below are ${clean.length} diagnosed lenses, each with an adversarial verdict. ` +
    `Synthesize a single ranked, sequenced implementation plan to close the ear-singularity gap with QuadriFlow. ` +
    `Weight by (expected impact x revised confidence / effort) and PENALIZE regression risk. Prefer survivors; a refuted lens may still ` +
    `contribute a salvaged idea — say so. Identify conflicts (fixes that must not be combined). Pick ONE topStep: the highest-ROI, lowest-risk ` +
    `first change, with the exact first edit and the exact command+metric delta that confirms it.\n\n` +
    `DIAGNOSED LENSES WITH VERDICTS (JSON):\n${JSON.stringify(clean, null, 2)}`,
  { label: 'synthesize', phase: 'Synthesize', schema: PLAN_SCHEMA },
)

return { model: MODEL, baseline: BASELINE, survivors: survivors.map((s) => s.lens), plan, allLenses: clean }
