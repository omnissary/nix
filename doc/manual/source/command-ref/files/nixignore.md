# `.nixignore`

A source tree can exclude files from the source Nix copies or hashes by
providing a `.nixignore` file at its root.

If the root of a fetched source contains a regular `.nixignore` file, its rules
select which of the source's files are used. If there is no such file, the
source is used exactly as it was before this mechanism existed.
The `.nixignore` file is itself always included in the source, and Nix rejects a
rule set that excludes it.

Nothing is recorded in `flake.lock`. Whether the policy applies is a property of
the fetched tree, so a pinned revision always selects the same files, and lock
entries stay readable by a Nix that does not know about `.nixignore`.

## Candidates

The policy narrows the set of files a fetcher already selected; it never adds
one.

For Git inputs the candidates are the files Git selects: the tracked files of
the working tree, or those of the fetched revision. Untracked files are not part
of a Git input. `.gitignore` and `.ignore` are deliberately **not** consulted:
Git has already applied them when it decided what to track, so reading them
again would drop files that were committed in spite of them.

A path-like reference to a directory inside a Git repository is a Git input
rooted at the repository, and a flake in a subdirectory is represented with
`dir=`, so revision metadata such as `self.rev` and dirty-tree detection are
unaffected by the policy. A working tree whose only modifications are excluded
by the policy is not dirty.

For path inputs every filesystem entry below the input root is a candidate. A
flake inside a Mercurial or Jujutsu work tree is a path input rooted at the
flake directory.

Every source applies its own policy. When Git submodules are enabled, a
submodule's own `.nixignore` governs its contents, and the superproject's policy
applies on top of the mounted result.

When a policy applies, directories or files named `.git`, `.hg`, or `.jj` are
excluded at every depth. This exclusion cannot be negated by a pattern.

## Pattern syntax

Patterns use Git ignore pattern semantics:

- blank lines and lines beginning with `#` are ignored;
- a leading `/` anchors a pattern at the source root;
- a trailing `/` matches directories only;
- `*`, `?`, character classes, POSIX character classes, and `**` are supported;
- a leading `!` negates a rule, and the last matching rule wins;
- `\#`, `\!`, and escaped trailing spaces represent literal characters.

Matching is case-sensitive.
Only the root `.nixignore` is read; nested ignore files do not add rules.
As with Git, a file cannot be re-included while one of its parent directories
remains excluded.

```gitignore
/network/
/pki/
*.log
!build/important.log
```

## Caveats

Adding a `.nixignore` changes the source Nix selects, and therefore the source
hash. Because it is a new file, existing lock entries pin revisions that do not
contain one and are unaffected; a lock entry that is updated onto a revision
that does contain one will hash differently than it did before, and than an
older Nix would.

`.nixignore` is a source-selection mechanism, not encryption.
It does not remove existing store paths or prevent an impure expression from
importing the same data through another path.
