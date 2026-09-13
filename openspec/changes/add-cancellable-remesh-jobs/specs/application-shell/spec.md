## MODIFIED Requirements

### Requirement: Long operations preserve the committed document
The application shell SHALL show progress for long-running operations and SHALL keep the last committed document visible until a successful operation has produced a complete replacement. Its Cancel control SHALL request cancellation from the underlying operation itself, not merely stop a progress observer. After cancellation or failure, the shell SHALL clear its busy state and retain the previously committed document.

#### Scenario: Cancelling from the iPad overlay
- **WHEN** a user selects Cancel while a remesh is running from the iPadOS progress overlay
- **THEN** the remesh job SHALL receive a cancellation request and the displayed document SHALL remain unchanged
