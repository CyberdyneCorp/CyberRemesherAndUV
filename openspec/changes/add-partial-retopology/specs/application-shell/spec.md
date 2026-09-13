## ADDED Requirements

### Requirement: Partial retopology commits as one document operation

A successful partial-retopology result SHALL replace the document EditMesh in
one undoable operation and invalidate only annotations whose report says their
identity or attribute transfer is unsupported. A rejected, failed or cancelled
operation SHALL create no undo entry and leave the document clean state and
EditMesh unchanged.

#### Scenario: Undo restores the pre-operation mesh

- **WHEN** a document commits partial retopology and the user undoes it
- **THEN** the exact pre-operation EditMesh and surviving annotations SHALL be
  restored as one command
