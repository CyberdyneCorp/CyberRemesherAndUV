#!/usr/bin/env python3
"""Regression guard for iPadOS remesh cancellation wiring.

The SwiftUI overlay calls AppModel.cancelOperation().  Cancelling its progress
consumer is intentionally insufficient: only RemeshOperation.cancel() reaches
the C ABI cancel callback.  Keep this source-level guard runnable on all CI
platforms, where the iPadOS shell itself cannot be compiled.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
APP_MODEL = REPO / "apps/mobile/ipados/Sources/CyberRemesherApp/AppModel.swift"


def main() -> None:
    source = APP_MODEL.read_text(encoding="utf-8")
    start = source.index("public func cancelOperation()")
    end = source.index("\n    private func updateProgress", start)
    implementation = source[start:end]
    assert "runningOp?.cancel()" in implementation, (
        "AppModel.cancelOperation() must cancel the RemeshOperation itself; "
        "cancelling only the progress-pump task does not reach CyberCancelCb"
    )


if __name__ == "__main__":
    main()
