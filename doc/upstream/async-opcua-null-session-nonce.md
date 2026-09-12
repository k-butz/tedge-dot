# Upstream issue draft: async-opcua server sends an empty session nonce on None-policy channels

Target: https://github.com/FreeOpcUa/async-opcua (crate `async-opcua-crypto` / `async-opcua-server` 0.18.0)

## Summary

`SecurityPolicy::random_nonce()` returns `ByteString::null()` for `SecurityPolicy::None`
(`async-opcua-crypto/src/security_policy.rs`), and the server uses it as the `serverNonce` of
`ActivateSessionResponse` (`async-opcua-server/src/session/manager.rs`, `activate_session`).
open62541 clients from 1.4 on require the session nonce to be at least 32 bytes regardless of
the security policy (`src/client/ua_client_connect.c`, "Session cannot be activated with a nonce
that is too short") and fail the connection with `BadSecurityChecksFailed`. An async-opcua server
with only a None endpoint is therefore unreachable for open62541-based clients.

## Reproduction

1. `ServerBuilder::new_anonymous("x")` with the default (None) endpoint.
2. Connect with an open62541 1.5.x client: `UA_Client_connect(client, "opc.tcp://127.0.0.1:4840")`.
3. Observe the client log: the secure channel and CreateSession succeed, ActivateSession's
   response carries a zero-length `serverNonce`, the client aborts with
   `BadSecurityChecksFailed`.

Other server stacks (open62541 server, python-asyncua, commercial servers) return a 32-byte
random session nonce on None channels, so the client behaviour is the common expectation.

## Suggested fix

Return a 32-byte random nonce for `SecurityPolicy::None` in `random_nonce()` (the session
nonce is an application-level nonce, independent of the secure-channel nonce length), or
generate the ActivateSession nonce from `ServerConfig::session_nonce_length` (already 32 by
default and used for CreateSession) instead of the policy.

## Local workaround

tedge-dot vendors `async-opcua-crypto` with that one-line change
(`vendor/async-opcua-crypto`, applied via `[patch.crates-io]`) so the conformance harness's
embedded simulator accepts the C connector (open62541). Found on 2026-09-11 while running the
conformance suite against the C build.
