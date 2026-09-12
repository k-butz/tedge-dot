# Vendored `async-opcua-crypto` 0.18.0 with one patch

Verbatim copy of the crates.io package, wired in through `[patch.crates-io]` in the workspace
`Cargo.toml`, with a single change in `src/security_policy.rs`:

`SecurityPolicy::None.random_nonce()` returns 32 random bytes instead of a null ByteString.

Why: the async-opcua **server** uses it for the `serverNonce` of `ActivateSessionResponse`.
On a None-policy channel upstream sends an empty nonce; open62541 clients (1.4+) require a
session nonce of at least 32 bytes regardless of the security policy and fail the connection
with `BadSecurityChecksFailed` ("Session cannot be activated with a nonce that is too short").
Every other server stack we tested against (open62541 server, python-asyncua, commercial
servers) sends 32 bytes, so the conformance harness's embedded simulator would otherwise reject
the most common open-source client — including the C tedge-dot connector.

Upstream report draft: `doc/upstream/async-opcua-null-session-nonce.md`. Drop this directory
and the `[patch.crates-io]` entry once a release with the fix is available.
