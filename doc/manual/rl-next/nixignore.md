---
synopsis: Filter flake sources with `.nixignore`
significance: significant
issues: 12291
---

A source tree can now exclude files from what Nix copies or hashes by providing
a [`.nixignore`](@docroot@/command-ref/files/nixignore.md) file at its root,
using Git ignore pattern syntax.

The policy only narrows what a fetcher already selects. Git inputs still
contribute their tracked files and keep their revision metadata, and
`.gitignore` is not consulted, since Git applied it when it chose what to track.

Nothing is recorded in `flake.lock`: whether the policy applies follows from the
fetched tree, so lock entries remain readable by a Nix without this feature.
