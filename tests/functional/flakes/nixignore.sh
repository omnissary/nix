#!/usr/bin/env bash

source ./common.sh

requireGit

# A shorthand local flake stays a Git input rooted at the repository, and the
# root .nixignore filters the files Git selected.
repo=$TEST_ROOT/nixignore-repo
createGitRepo "$repo"
mkdir -p "$repo/secret" "$repo/sub"

cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    visible = builtins.readFile ./visible;
    rev = self.rev or "none";
  };
}
EOF
cat > "$repo/sub/flake.nix" <<'EOF'
{
  outputs = { self }: { source = self.outPath; };
}
EOF
echo public > "$repo/visible"
echo private > "$repo/secret/value"
echo /secret/ > "$repo/.nixignore"
echo /junk > "$repo/.gitignore"
git -C "$repo" add flake.nix sub/flake.nix visible secret/value .nixignore .gitignore
git -C "$repo" commit -m initial
echo untracked > "$repo/untracked"

localMetadata=$(nix flake metadata --refresh --json "$repo")
source1=$(nix flake prefetch --refresh --json "$repo" | jq -r .storePath)
[[ $(jq -r .locked.type <<< "$localMetadata") = git ]]
test -e "$source1/flake.nix"
test -e "$source1/.nixignore"
test ! -e "$source1/secret"
test ! -e "$source1/.git"
[[ $(nix eval --raw "$repo#visible") = public ]]

# Nothing is recorded in the lock: the policy is a property of the source, so
# lock entries stay readable by a Nix that does not know about it.
[[ $(jq -r '.locked.nixIgnore // "none"' <<< "$localMetadata") = none ]]

# Git metadata survives: a shorthand reference is not downgraded to a path
# snapshot, so the revision is still available and untracked files stay out.
[[ $(jq -r '.locked.rev // "none"' <<< "$localMetadata") != none ]]
[[ $(nix eval --raw "$repo#rev") != none ]]
test ! -e "$source1/untracked"

# A shorthand reference and the same revision fetched over git+file: select
# the same files.
explicitGit="git+file://$repo?ref=master"
gitSource=$(nix flake prefetch --refresh --json "$explicitGit" | jq -r .storePath)
[[ $(nix flake metadata --refresh --json "$explicitGit" | jq -r .locked.rev) != null ]]
[[ $source1 = "$gitSource" ]]

# Only .nixignore is read. Git already applied .gitignore when it chose what to
# track, so a tracked file that .gitignore matches stays in the source.
forced=$TEST_ROOT/nixignore-forced
createGitRepo "$forced"
cat > "$forced/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
echo generated > "$forced/.gitignore"
echo kept > "$forced/generated"
git -C "$forced" add -f flake.nix .gitignore generated
git -C "$forced" commit -m initial
test -e "$(nix flake prefetch --refresh --json "$forced" | jq -r .storePath)/generated"
test -e "$(nix flake prefetch --refresh --json "git+file://$forced?ref=master" | jq -r .storePath)/generated"

# The ignore root stays at the repository root for a flake in a subdirectory.
subMetadata=$(nix flake metadata --refresh --json "$repo/sub")
subSource=$(nix flake prefetch --refresh --json "$repo/sub" | jq -r .storePath)
[[ $(jq -r .locked.type <<< "$subMetadata") = git ]]
[[ $(jq -r .locked.dir <<< "$subMetadata") = sub ]]
test ! -e "$subSource/secret"
test -e "$subSource/sub/flake.nix"

# Ignored content affects neither the source hash nor lastModified, and does
# not make the working tree dirty.
fingerprint1=$(jq -r .fingerprint <<< "$localMetadata")
lastModified1=$(jq -r .locked.lastModified <<< "$localMetadata")
echo changed-private > "$repo/secret/value"
touch -d @2000000000 "$repo/secret/value"
localMetadata2=$(nix flake metadata --refresh --json "$repo")
[[ $(jq -r .fingerprint <<< "$localMetadata2") = "$fingerprint1" ]]
[[ $(jq -r .locked.lastModified <<< "$localMetadata2") = "$lastModified1" ]]
[[ $(jq -r '.locked.rev // "none"' <<< "$localMetadata2") != none ]]
git -C "$repo" checkout -- secret/value

echo '# policy change' >> "$repo/.nixignore"
policyFingerprint=$(nix flake metadata --refresh --json "$repo" | jq -r .fingerprint)
[[ $fingerprint1 != "$policyFingerprint" ]]

echo changed-public > "$repo/visible"
fingerprint3=$(nix flake metadata --refresh --json "$repo" | jq -r .fingerprint)
[[ $policyFingerprint != "$fingerprint3" ]]
git -C "$repo" checkout -- visible .nixignore

# Generic fetchers apply the same policy; there is nothing to opt into.
treeJson=$(nix eval --impure --json --expr \
    "let tree = builtins.fetchTree { type = \"path\"; path = \"$repo\"; }; in {
      path = tree.outPath;
      hasNixIgnore = tree ? nixIgnore;
    }")
tree=$(jq -r .path <<< "$treeJson")
[[ $(jq -r .hasNixIgnore <<< "$treeJson") = false ]]
test ! -e "$tree/secret"
test ! -e "$tree/.git"

fetchedGit=$(nix eval --impure --raw --expr \
    "(builtins.fetchGit { url = \"file://$repo\"; }).outPath")
test ! -e "$fetchedGit/secret"

# For a path source every entry is a candidate, but only .nixignore selects.
policySource=$TEST_ROOT/nixignore-path
mkdir -p "$policySource/nested/.git" "$policySource/nested/.hg" "$policySource/nested/.jj"
cat > "$policySource/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
touch "$policySource/git-only" "$policySource/ignore-only" "$policySource/nix-only" "$policySource/nested/visible"
touch "$policySource/nested/.git/config" "$policySource/nested/.hg/requires" "$policySource/nested/.jj/repo"
echo /git-only > "$policySource/.gitignore"
echo /ignore-only > "$policySource/.ignore"
echo /nix-only > "$policySource/.nixignore"
echo /nested/visible > "$policySource/nested/.nixignore"
policyStore=$(nix flake prefetch --refresh --json "path:$policySource" | jq -r .storePath)
test ! -e "$policyStore/nix-only"
test -e "$policyStore/git-only"
test -e "$policyStore/ignore-only"
test -e "$policyStore/nested/visible"
test ! -e "$policyStore/nested/.git"
test ! -e "$policyStore/nested/.hg"
test ! -e "$policyStore/nested/.jj"

# A path source without a .nixignore keeps its previous contents.
noPolicy=$TEST_ROOT/nixignore-no-policy
mkdir -p "$noPolicy"
echo '{ outputs = _: {}; }' > "$noPolicy/flake.nix"
echo /flake.nix > "$noPolicy/.gitignore"
noPolicyStore=$(nix flake prefetch --refresh --json "path:$noPolicy" | jq -r .storePath)
test -e "$noPolicyStore/flake.nix"
test -e "$noPolicyStore/.gitignore"
[[ $(nix flake metadata --json "path:$noPolicy" | jq -r '.locked.nixIgnore // "none"') = none ]]

# Compare the matcher against Git over a representative wildmatch corpus.
corpus=$TEST_ROOT/nixignore-wildmatch
createGitRepo "$corpus"
cat > "$corpus/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
cat > "$corpus/.nixignore" <<'EOF'
/root-only
anywhere
dir/file
*.tmp
file?.txt
item[0-2]
dash[a\-z]
a/**/generated
cache/**
build/
!build
/build/*
!/build/keep
private/
!private/public.txt
file[[:digit:]].txt
**/deep
foo/**/bar
\#hash
\!bang
EOF
printf '%s\n' 'literal\ ' >> "$corpus/.nixignore"
cp "$corpus/.nixignore" "$corpus/.gitignore"
corpusFiles=(
    root-only nested/root-only nested/anywhere dir/file nested/dir/file
    one.tmp nested/two.tmp file1.txt file10.txt item2 item3 dash- dashm
    a/generated a/b/c/generated cache/child cache/deep/child
    build/drop build/keep/child private/public.txt
    file7.txt filex.txt deep nested/deep foo/bar foo/a/b/bar
    '#hash' '!bang' 'literal ' visible
)
for relative in "${corpusFiles[@]}"; do
    mkdir -p "$corpus/$(dirname "$relative")"
    echo candidate > "$corpus/$relative"
done
corpusStore=$(nix flake prefetch --refresh --json "path:$corpus" | jq -r .storePath)
for relative in "${corpusFiles[@]}"; do
    if git -C "$corpus" check-ignore --no-index --quiet -- "$relative"; then
        test ! -e "$corpusStore/$relative"
    else
        test -e "$corpusStore/$relative"
    fi
done

# Every source declares its own policy: a submodule's .nixignore governs its
# own contents, and the superproject's policy applies on top of the mount.
subRepo=$TEST_ROOT/nixignore-submodule
createGitRepo "$subRepo"
echo ignored > "$subRepo/ignored"
echo kept > "$subRepo/kept"
echo survives > "$subRepo/survives"
echo /kept > "$subRepo/.nixignore"
git -C "$subRepo" add ignored kept survives .nixignore
git -C "$subRepo" commit -m initial

superRepo=$TEST_ROOT/nixignore-super
createGitRepo "$superRepo"
cat > "$superRepo/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
mkdir -p "$superRepo/deps"
git -c protocol.file.allow=always -C "$superRepo" submodule add "$subRepo" deps/sub
echo /deps/sub/ignored > "$superRepo/.nixignore"
git -C "$superRepo" add flake.nix .nixignore .gitmodules deps/sub
git -C "$superRepo" commit -m initial

superSource=$(nix flake prefetch --refresh --json \
    "git+file://$superRepo?ref=master&submodules=1" | jq -r .storePath)
test ! -e "$superSource/deps/sub/ignored"
test ! -e "$superSource/deps/sub/kept"
test -e "$superSource/deps/sub/survives"
test -e "$superSource/deps/sub/.nixignore"

echo /deps/sub/ > "$superRepo/.nixignore"
git -C "$superRepo" add .nixignore
git -C "$superRepo" commit -m skip-submodule
superSource=$(nix flake prefetch --refresh --json \
    "git+file://$superRepo?ref=master&submodules=1" | jq -r .storePath)
test ! -e "$superSource/deps/sub"

# Only a Git root widens a shorthand reference to the whole repository. A flake
# inside a Mercurial or Jujutsu work tree is a path source rooted at the flake
# directory, so the policy is read from there.
hgRoot=$TEST_ROOT/nixignore-hg
if [[ -n $(type -p hg) ]]; then
    mkdir -p "$hgRoot/sub" "$hgRoot/hidden"
    hg init "$hgRoot"
    echo '{ outputs = _: {}; }' > "$hgRoot/sub/flake.nix"
    echo hidden > "$hgRoot/hidden/value"
    echo /dropped > "$hgRoot/sub/.nixignore"
    echo dropped > "$hgRoot/sub/dropped"
    echo kept > "$hgRoot/sub/kept"
    hgMetadata=$(nix flake metadata --refresh --json "$hgRoot/sub")
    hgSource=$(nix flake prefetch --refresh --json "$hgRoot/sub" | jq -r .storePath)
    [[ $(jq -r .locked.type <<< "$hgMetadata") = path ]]
    [[ $(jq -r '.locked.dir // "none"' <<< "$hgMetadata") = none ]]
    test -e "$hgSource/kept"
    test ! -e "$hgSource/dropped"
    test ! -e "$hgSource/hidden"
fi

jjRoot=$TEST_ROOT/nixignore-jj
mkdir -p "$jjRoot/.jj/repo" "$jjRoot/sub"
echo '{ outputs = _: {}; }' > "$jjRoot/sub/flake.nix"
echo hidden > "$jjRoot/untracked"
echo /dropped > "$jjRoot/sub/.nixignore"
echo dropped > "$jjRoot/sub/dropped"
echo kept > "$jjRoot/sub/kept"
jjMetadata=$(nix flake metadata --refresh --json "$jjRoot/sub")
jjSource=$(nix flake prefetch --refresh --json "$jjRoot/sub" | jq -r .storePath)
[[ $(jq -r .locked.type <<< "$jjMetadata") = path ]]
[[ $(jq -r '.locked.dir // "none"' <<< "$jjMetadata") = none ]]
test -e "$jjSource/kept"
test ! -e "$jjSource/dropped"
test ! -e "$jjSource/untracked"
