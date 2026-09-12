## 1. Parity repair

- [x] 1.1 Capture the validated C ABI adaptivity value in the shared extractor
      factory and forward it to the quad-cover and ZRemesher selector paths.
- [x] 1.2 Remove documentation that describes the C ABI override as an intended
      uniform policy; document the compatible explicit `0.0` migration path.

## 2. Regression coverage

- [x] 2.1 Add C ABI regressions proving explicit `0.0` and `1.0` reach the
      default quad-cover and selector-based ZRemesher extractors.
- [x] 2.2 Preserve deterministic same-value coverage and verify the regression
      fails when either forwarding path is restored to its hardcoded value.
- [x] 2.3 Make the open-surface uniformity regression request its required
      `adaptivity=0.0` explicitly, rather than relying on the former ABI override.

## 3. Verification and release notes

- [x] 3.1 Run focused C ABI and full cpu-headless tests with the shipping
      native+Geogram configuration, retaining the count-calibration regression.
- [x] 3.2 Update CHANGELOG.md with the C ABI default-output migration and
      validate the OpenSpec change strictly.
