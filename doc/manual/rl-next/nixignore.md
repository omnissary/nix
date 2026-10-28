---
synopsis: Filter flake sources with `.nixignore`
significance: significant
issues: 12291
---

A source tree can now exclude files from what Nix copies or hashes by providing
a [`.nixignore`](@docroot@/command-ref/files/nixignore.md) file at its root,
using Git ignore pattern syntax.

The policy only narrows what a fetcher already selects. Git and Mercurial inputs
still contribute their tracked files and keep their revision metadata, and
`.gitignore` and `.hgignore` are not consulted, since they were already applied
when the version control system chose what to track.

Nothing is recorded in `flake.lock`: whether the policy applies follows from the
fetched tree, so lock entries remain readable by a Nix without this feature.
