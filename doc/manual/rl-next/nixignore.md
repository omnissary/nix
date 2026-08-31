---
synopsis: Filter flake sources with `.nixignore`
significance: significant
issues: 12291
---

Git and path flake inputs now honor a root [`.nixignore`](@docroot@/command-ref/files/nixignore.md)
before hashing or copying their source.
Git inputs keep their existing tracked-file selection and apply `.nixignore` as an additional exclusion layer,
allowing tracked local-only data to stay out of the Nix store.

The resolved input records `nixIgnore = 1`.
Existing lock entries without this marker preserve their old source identity and remain unfiltered until updated.
