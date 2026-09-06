---
synopsis: Filter flake sources with ignore files
significance: significant
issues: 12291
---

Flake source selection now reads a root [`.nixignore`, `.ignore`, or `.gitignore`](@docroot@/command-ref/files/nixignore.md)
before hashing or copying Git and path inputs.
Local shorthand references inside Git, Mercurial, and Jujutsu work trees are path snapshots rooted at the nearest repository,
so untracked files are included unless the selected policy excludes them.
Use an explicit `git+file:` reference to retain Git's tracked-file selection.
Git-specific query parameters on a shorthand reference retain Git behavior as well.

Resolved inputs record `nixIgnore = 1` even when no policy file is present.
Existing lock entries without this marker retain legacy behavior until updated.
Generic fetchers do not discover ignore files automatically; pass `nixIgnore = 1` to opt in.
