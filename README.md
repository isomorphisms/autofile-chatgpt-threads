# autofile-chatgpt-threads

Native API boundary for filing an existing ChatGPT conversation into an
existing ChatGPT Project.

This repository does not classify conversations. Its caller supplies an exact
conversation ID, destination Project ID, expected current Project ID and an
approval digest. The library discovers Projects, reads conversation state,
issues at most one move request, then independently reads the conversation
again before it reports a verified move.

The `chatgpt.com/backend-api` routes are private and unversioned. Authentication
and HTTP belong to an injected, same-origin transport. The library accepts no
cookie, bearer token, arbitrary URL, arbitrary method or arbitrary request
body.

```text
make check ICK=/verified/ick
```

No test contacts ChatGPT or reads user data.

Maintained builds require the qualified ICK producer declared in
`ci/build-toolchain.tsv`. A generic host compiler may be used only for an
explicitly labelled local diagnostic; it is not acceptance evidence.

