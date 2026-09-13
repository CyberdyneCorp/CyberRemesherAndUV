import Foundation
import XCTest

@testable import CyberRemesher

private enum JobTestError: Error, Equatable {
    case cancelled
}

private final class LockedCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var storage = 0

    func increment() {
        lock.lock()
        storage += 1
        lock.unlock()
    }

    var value: Int {
        lock.lock()
        defer { lock.unlock() }
        return storage
    }
}

final class RemeshJobTests: XCTestCase {
    func testConcurrentAwaitersShareOneExecutionAndResult() async throws {
        let executions = LockedCounter()
        let job = RemeshJob<Int>(
            work: {
                executions.increment()
                Thread.sleep(forTimeInterval: 0.03)
                return .success(42)
            },
            didFinish: {},
            requestCancellation: {}
        )

        async let first = job.value()
        async let second = job.value()
        let firstResult = try await first
        let secondResult = try await second
        XCTAssertEqual(firstResult, 42)
        XCTAssertEqual(secondResult, 42)
        XCTAssertEqual(executions.value, 1)

        let repeatedResult = try await job.value()
        XCTAssertEqual(repeatedResult, 42)
        XCTAssertEqual(executions.value, 1)
    }

    func testExplicitCancellationIsIdempotentAndTerminalResultIsShared() async {
        let cancellations = LockedCounter()
        let job = RemeshJob<Int>(
            work: { .failure(JobTestError.cancelled) },
            didFinish: {},
            requestCancellation: { cancellations.increment() }
        )

        job.cancel()
        job.cancel()

        for _ in 0..<2 {
            do {
                _ = try await job.value()
                XCTFail("cancelled job unexpectedly succeeded")
            } catch let error as JobTestError {
                XCTAssertEqual(error, .cancelled)
            } catch {
                XCTFail("unexpected error: \(error)")
            }
        }
        XCTAssertEqual(cancellations.value, 1)
    }

    func testTaskCancellationRequestsTheSharedJobCancellation() async {
        let cancellations = LockedCounter()
        let job = RemeshJob<Int>(
            work: { .failure(JobTestError.cancelled) },
            didFinish: {},
            requestCancellation: { cancellations.increment() }
        )
        let task = Task { try await job.value() }
        task.cancel()

        do {
            _ = try await task.value
            XCTFail("cancelled task unexpectedly succeeded")
        } catch let error as JobTestError {
            XCTAssertEqual(error, .cancelled)
        } catch {
            XCTFail("unexpected error: \(error)")
        }
        XCTAssertEqual(cancellations.value, 1)
    }
}
