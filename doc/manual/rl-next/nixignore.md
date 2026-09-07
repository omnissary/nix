---
synopsis: Filter flake sources with ignore files
significance: significant
issues: 12291
---

Flake source selection now reads a root [`.nixignore`, `.ignore`, or `.gitignore`](@docroot@/command-ref/files/nixignore.md)
before hashing or copying Git and path inputs.
The policy only narrows what a fetcher already selects:
Git inputs still contribute their tracked files, keep their revision metadata, and read only `.nixignore`,
since Git applied its own ignore files when it chose what to track.

Resolved inputs record `nixIgnore = 1` even when no policy file is present.
Existing lock entries without this marker retain legacy behavior until updated.
Generic fetchers do not discover ignore files automatically; pass `nixIgnore = 1` to opt in.
