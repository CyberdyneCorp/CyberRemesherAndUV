# network-bridge Specification

## Purpose
An opt-in local-network protocol so another application can drive this engine
in place, plus the reference client that exercises it. It exists for the tablet
and DCC-plugin cases where the mesh already lives in another process and
copying it through files is the wrong shape. Opt-in, versioned and local by
default: a bridge that is on when nobody asked for it is a security problem,
not a feature.
## Requirements
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

Containment is per connection: no peer input SHALL be able to end the **process**. Bytes received from a peer SHALL NOT be echoed back into a reply, and every reply SHALL be serializable by construction — an encoding error on the error path is still an error path. A connection handler SHALL catch everything, so an unexpected exception costs that connection and nothing else. The declared size ceiling SHALL be enforced before a message is buffered and SHALL bind **both** peers: a client SHALL validate the server's replies exactly as the server validates the client's requests. Connection resources SHALL be reclaimed as connections end, and a failed thread spawn SHALL NOT abort the server.

#### Scenario: Incompatible client
- **WHEN** a client with an unsupported protocol version connects
- **THEN** the app SHALL refuse the session and report both versions to the client

#### Scenario: A non-UTF-8 byte costs the connection, not the process
- **WHEN** a client sends a request containing bytes that are not valid UTF-8 — Latin-1 text is enough, no attacker required
- **THEN** the server SHALL reply with an error or close that connection, the host process SHALL survive, and the reply SHALL NOT contain the peer's bytes

#### Scenario: An oversized declaration is refused before it is buffered
- **WHEN** either peer receives a length prefix larger than the protocol's ceiling
- **THEN** it SHALL fail with a typed error naming the limit, without allocating for the declared size, and the reference Python client SHALL behave identically to the C++ implementations

#### Scenario: A long-lived server does not leak per connection
- **WHEN** many connections are opened and closed against a running bridge
- **THEN** every connection's thread and buffers SHALL be reclaimed, and a thread the OS refuses to create SHALL be reported as a failed connection rather than aborting the server

