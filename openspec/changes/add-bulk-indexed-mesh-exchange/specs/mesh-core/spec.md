## ADDED Requirements

### Requirement: Transactional indexed construction

The mesh core SHALL support transactional construction from validated indexed
polygons such that a failed import cannot expose partially constructed topology.

#### Scenario: Retained unused vertex

- **WHEN** an indexed import contains a valid vertex referenced by no face
- **THEN** the imported mesh SHALL retain that vertex in its compacted export
