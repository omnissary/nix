# `.nixignore`

When the [`flakes`](@docroot@/development/experimental-features.md#xp-feature-flakes) feature is enabled,
a regular file named `.nixignore` at the root of a `git:` or `path:` input excludes paths before Nix hashes or copies the source.

For Git inputs, `.nixignore` is an additional filter: only files already selected by the Git fetcher are candidates.
It does not make untracked files visible.
For path inputs, all filesystem entries are candidates.
Nix includes `.nixignore` itself in the source, and changing it changes the source identity.

The `.nixignore` file must itself belong to the fetcher's candidate set.
In particular, add it to the Git index before relying on it in a local Git input.
Nix rejects a rule set that excludes `.nixignore` itself.

Version 1 supports Git ignore pattern syntax:

- blank lines and lines beginning with `#` are ignored;
- a leading `/` anchors a pattern at the input root;
- a trailing `/` matches directories only;
- `*`, `?`, character classes, and `**` are supported;
- a leading `!` negates a rule, and the last matching rule wins;
- `\#`, `\!`, and escaped trailing spaces represent literal characters.

Matching is case-sensitive.
Rules come only from the root `.nixignore`; nested files and `.ignore` or `.gitignore` fallbacks are not read.
As with Git, a file cannot be re-included when one of its parent directories remains excluded.

```gitignore
/network/
/pki/
*.log
!build/important.log
```

The effective root is the source-input root (the Git repository root or `path:` root),
including when a flake reference uses `dir=`.
Version 1 does not support Git inputs with `submodules = true`.

Resolved and locked inputs record `nixIgnore = 1`.
Older lock entries remain unfiltered for compatibility; update the lock file to opt them into filtering.

`.nixignore` is a source-selection mechanism, not encryption.
It does not remove existing store paths or stop an impure expression from importing the same data through another path.
