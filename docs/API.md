# API boundary

`autofile_api` owns only the filing transport boundary. Classification and
destination selection are upstream concerns.

The injected transport must bind every response to a stable fingerprint of the
selected ChatGPT account/workspace. It may acquire page-local authentication,
but tokens and cookies never enter this library or its receipts.

The private routes currently modeled are:

| Primitive | Request |
| --- | --- |
| `af_list_projects` | `GET /backend-api/gizmos/snorlax/sidebar?owned_only=false&conversations_per_gizmo=0&limit=20[&cursor=…]` |
| `af_list_project_threads` | `GET /backend-api/gizmos/{project_id}/conversations?cursor=…` |
| `af_get_thread` | `GET /backend-api/conversation/{thread_id}` |
| `af_move_thread` | exactly one `PATCH /backend-api/conversation/{thread_id}` with `{"gizmo_id":"g-p-…"}`, followed by `af_get_thread` |

The routes are private, unversioned ChatGPT implementation details. A changed
shape fails as `schema_changed`; it never broadens into arbitrary fetch.

## Move states

- `dry_run`: destination, write permission, thread identity, eligibility and
  source precondition verified; no PATCH sent.
- `verified_noop`: the fresh thread read already names the destination.
- `verified`: one PATCH was attempted and the independent post-read names the
  exact thread and destination.
- `unknown`: the write outcome or postcondition is unresolved. Never retry the
  PATCH blindly.
- `blocked`: validation failed before a verified outcome.

The approval digest is bound by exact comparison to an independently supplied
expected digest. A production controller must derive and admit that digest; the
library does not self-approve moves.

## Evidence basis

The endpoint family and move semantics are preserved in the pinned private-web
API reference at `dilapidated-shed/ChatGPTExporter` and its mirror of
`dilapidated-shed/chatgpt-cli@254982ab784e4d13c7ab5f87de1522f9b047d0f5`.
Official ChatGPT help documents the supported visible **Move to project** action,
but does not document these backend routes.

