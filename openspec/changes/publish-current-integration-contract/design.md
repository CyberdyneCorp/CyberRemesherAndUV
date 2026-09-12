## Context

Historical plans, README detail and binding READMEs serve different audiences.

## Decisions

- Add a compact `docs/CURRENT_STATUS.md` rather than rewriting the research log.
- Link it from high-discovery documentation and cite the C ABI as the integration
  boundary.

## Risks / Trade-offs

- [Status drifts] → Link to issue #46 and normative OpenSpec specs; update this
  document whenever a platform artifact or verification boundary changes.
