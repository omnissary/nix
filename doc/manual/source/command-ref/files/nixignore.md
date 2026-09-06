# `.nixignore`

When Nix loads a [flake](@docroot@/command-ref/new-cli/nix3-flake.md),
it applies a versioned source-selection policy before hashing or copying local source trees.
The policy reads the first regular file that exists at the source root, in this order:

1. `.nixignore`
2. `.ignore`
3. `.gitignore`

If none of these files exists, all candidate files are included.
The selected policy file is itself included in the source,
and Nix rejects a rule set that excludes it.
Changing the policy file therefore changes the source identity.

Local path-like flake references inside a Git, Mercurial, or Jujutsu working tree use the nearest VCS root as the source root.
They are snapshotted as path inputs, so untracked files are candidates too.
A flake in a subdirectory is represented with `dir=`; the ignore rules still apply from the source root.
Use an explicit `git+file:` reference when Git's tracked-file selection and revision metadata are desired instead.
For compatibility, Git-specific query parameters such as `ref=` or `submodules=1` on a shorthand reference also select Git behavior.

For other path inputs, all filesystem entries below the input root are candidates.
For explicit Git inputs, only entries selected by the Git fetcher are candidates before the ignore policy is applied.
When Git submodules are enabled, the root policy is also applied to the mounted submodule contents;
policy files inside submodules are not read.

Directories or files named `.git`, `.hg`, or `.jj` are always excluded at every depth.
This exclusion cannot be negated by a pattern.

## Pattern syntax

Version 1 uses Git ignore pattern semantics:

- blank lines and lines beginning with `#` are ignored;
- a leading `/` anchors a pattern at the source root;
- a trailing `/` matches directories only;
- `*`, `?`, character classes, POSIX character classes, and `**` are supported;
- a leading `!` negates a rule, and the last matching rule wins;
- `\#`, `\!`, and escaped trailing spaces represent literal characters.

Matching is case-sensitive.
Only the selected root policy file is read; nested ignore files do not add rules.
As with Git, a file cannot be re-included while one of its parent directories remains excluded.

```gitignore
/network/
/pki/
*.log
!build/important.log
```

## Activation and compatibility

Flake loading enables source-selection discovery automatically for Git and path inputs.
Resolved and locked directory inputs record `nixIgnore = 1`, including when no policy file exists.
This records the source-selection version and keeps subsequent fetches deterministic.

Generic fetchers such as `builtins.fetchTree` and `builtins.fetchGit` retain their previous behavior by default.
Pass `nixIgnore = 1` to opt in explicitly.

Existing final lock entries without `nixIgnore` retain their legacy source selection for compatibility.
Updating such an entry opts it into version 1.
For a shorthand local flake in a VCS working tree, that update can change the locked input from `git` to `path`
and remove Git revision attributes.

`.nixignore` is a source-selection mechanism, not encryption.
It does not remove existing store paths or prevent an impure expression from importing the same data through another path.
