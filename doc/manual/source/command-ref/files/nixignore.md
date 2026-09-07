# `.nixignore`

When Nix loads a [flake](@docroot@/command-ref/new-cli/nix3-flake.md),
it applies a versioned source-selection policy before hashing or copying local source trees.
The policy reads the first regular file that exists at the source root, in this order:

1. `.nixignore`
2. `.ignore`
3. `.gitignore`

Git inputs are the exception: only `.nixignore` is read.
Git has already applied `.gitignore` and `.ignore` when it decided which files to track,
so consulting them again would drop files that were committed in spite of them.

If no policy file applies, all candidate files are included.
The selected policy file is itself included in the source,
and Nix rejects a rule set that excludes it.
Changing the policy file therefore changes the source identity.

## Candidates

The policy narrows a set of candidate files; it never adds one.

For Git inputs the candidates are the files the Git fetcher selects:
the tracked files of the working tree, or those of the fetched revision.
Untracked files are not part of a Git input.
A path-like reference to a directory inside a Git repository is a Git input rooted at the repository,
and a flake in a subdirectory is represented with `dir=`,
so revision metadata such as `self.rev` and dirty-tree detection are unaffected by the policy.
A working tree whose only modifications are excluded by the policy is not dirty.

For path inputs every filesystem entry below the input root is a candidate.
A flake inside a Mercurial or Jujutsu work tree is a path input rooted at the flake directory.

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

`.nixignore` is a source-selection mechanism, not encryption.
It does not remove existing store paths or prevent an impure expression from importing the same data through another path.
