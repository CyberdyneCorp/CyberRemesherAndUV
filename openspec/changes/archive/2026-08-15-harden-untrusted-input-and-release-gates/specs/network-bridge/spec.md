# network-bridge (delta)

## MODIFIED Requirements

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
