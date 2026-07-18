# network-bridge — Local DCC Integration

## ADDED Requirements

### Requirement: Opt-in local-network protocol
The application SHALL offer a documented, versioned local-network protocol, disabled by default and enabled per session by explicit user action. When enabled it SHALL bind to the local network only; no cloud endpoint or external relay SHALL exist. The UI SHALL show a visible indicator whenever the bridge is listening.

#### Scenario: Off by default
- **WHEN** the application starts fresh
- **THEN** the bridge SHALL be off and no port SHALL be open

### Requirement: Protocol command set
The protocol SHALL support at minimum: push Target geometry (with vertex colors, UVs, and textures), push/load an EditMesh, pull the current EditMesh (with UVs), clear scene, close document, display a message to the user, create/delete remote action buttons and receive their presses, query symmetry state, query whether the EditMesh changed since a marker, and stream the viewport camera pose.

#### Scenario: Push and pull round trip
- **WHEN** a client pushes a Target mesh and later pulls the EditMesh
- **THEN** the Target SHALL appear in the open document and the pulled EditMesh SHALL match what the user built, including UVs

#### Scenario: Remote action button
- **WHEN** a client registers a remote action button and the user taps it
- **THEN** the client SHALL receive the press event

### Requirement: Reference Python client
The project SHALL ship a pip-installable Python client implementing the full protocol with a documented API and a CLI, serving as the integration point for DCC plugins (Blender addon and others consume this client rather than raw sockets).

#### Scenario: Scripted push
- **WHEN** a script calls the client's push-target function with an OBJ path against a listening app
- **THEN** the mesh SHALL load as the Target and the call SHALL report success

### Requirement: Protocol versioning and safety
Every connection SHALL begin with a version handshake; incompatible clients SHALL be rejected with a clear message. Malformed or oversized messages SHALL be rejected without crashing the application, and a misbehaving client SHALL not block the UI thread.

#### Scenario: Incompatible client
- **WHEN** a client with an unsupported protocol version connects
- **THEN** the app SHALL refuse the session and report both versions to the client
