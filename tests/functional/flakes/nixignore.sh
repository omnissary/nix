#!/usr/bin/env bash

source ./common.sh

requireGit

# A shorthand local flake stays a Git input rooted at the repository, and the
# root policy filters the files Git selected.
repo=$TEST_ROOT/nixignore-repo
createGitRepo "$repo"
mkdir -p "$repo/secret" "$repo/sub"
git -C "$repo" config filter.nixignore.clean "sed 's/private/encrypted/g'"
git -C "$repo" config filter.nixignore.smudge "sed 's/encrypted/private/g'"

cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    visible = builtins.readFile ./visible;
    rev = self.rev or "none";
    source = self.outPath;
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
echo '/secret/value filter=nixignore' > "$repo/.gitattributes"
git -C "$repo" add flake.nix sub/flake.nix visible secret/value .nixignore .gitignore .gitattributes
git -C "$repo" commit -m initial
echo untracked > "$repo/untracked"
[[ $(git -C "$repo" show HEAD:secret/value) = encrypted ]]
[[ $(< "$repo/secret/value") = private ]]

localMetadata=$(nix flake metadata --refresh --json "$repo")
source1=$(nix flake prefetch --refresh --json "$repo" | jq -r .storePath)
[[ $(jq -r .locked.type <<< "$localMetadata") = git ]]
[[ $(jq -r .locked.nixIgnore <<< "$localMetadata") = 1 ]]
test -e "$source1/flake.nix"
test -e "$source1/.nixignore"
test ! -e "$source1/secret"
test ! -e "$source1/.git"
[[ $(nix eval --raw "$repo#visible") = public ]]

# Git metadata survives: a shorthand reference is not downgraded to a path
# snapshot, so the revision is still available and untracked files stay out.
[[ $(jq -r '.locked.rev // "none"' <<< "$localMetadata") != none ]]
[[ $(nix eval --raw "$repo#rev") != none ]]
test ! -e "$source1/untracked"

# A shorthand reference and the same revision fetched over git+file: select
# the same files.
explicitGit="git+file://$repo?ref=master"
gitMetadata=$(nix flake metadata --refresh --json "$explicitGit")
gitSource=$(nix flake prefetch --refresh --json "$explicitGit" | jq -r .storePath)
[[ $(jq -r .locked.type <<< "$gitMetadata") = git ]]
[[ $(jq -r .locked.nixIgnore <<< "$gitMetadata") = 1 ]]
[[ $(jq -r .locked.rev <<< "$gitMetadata") != null ]]
test ! -e "$gitSource/untracked"
test ! -e "$gitSource/secret"
test ! -e "$gitSource/.git"
[[ $source1 = "$gitSource" ]]

# Git already applied its own ignore files when it chose what to track, so a
# Git input must not consult them again: a tracked file that .gitignore
# matches stays in the source.
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
test ! -e "$subSource/untracked"
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

# Generic fetchers retain legacy behavior unless nixIgnore is explicit.
legacyTreeJson=$(nix eval --impure --json --expr \
    "let tree = builtins.fetchTree { type = \"path\"; path = \"$repo\"; }; in {
      path = tree.outPath;
      hasNixIgnore = tree ? nixIgnore;
    }")
legacyTree=$(jq -r .path <<< "$legacyTreeJson")
[[ $(jq -r .hasNixIgnore <<< "$legacyTreeJson") = false ]]
test -e "$legacyTree/secret/value"
test -e "$legacyTree/.git"

filteredTreeJson=$(nix eval --impure --json --expr \
    "let tree = builtins.fetchTree { type = \"path\"; path = \"$repo\"; nixIgnore = 1; }; in {
      path = tree.outPath;
      nixIgnore = tree.nixIgnore;
    }")
filteredTree=$(jq -r .path <<< "$filteredTreeJson")
[[ $(jq -r .nixIgnore <<< "$filteredTreeJson") = 1 ]]
test ! -e "$filteredTree/secret"
test ! -e "$filteredTree/.git"

legacyGit=$(nix eval --impure --raw --expr \
    "(builtins.fetchGit { url = \"file://$repo\"; }).outPath")
test -e "$legacyGit/secret/value"

filteredGitJson=$(nix eval --impure --json --expr \
    "let tree = builtins.fetchGit { url = \"file://$repo\"; nixIgnore = 1; }; in {
      path = tree.outPath;
      nixIgnore = tree.nixIgnore;
    }")
filteredGit=$(jq -r .path <<< "$filteredGitJson")
[[ $(jq -r .nixIgnore <<< "$filteredGitJson") = 1 ]]
test ! -e "$filteredGit/secret"

# Policy files use .nixignore, .ignore, .gitignore precedence.
policySource=$TEST_ROOT/nixignore-precedence
mkdir -p "$policySource/nested"
cat > "$policySource/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
touch "$policySource/git-only" "$policySource/ignore-only" "$policySource/nix-only" "$policySource/nested/visible"
echo /git-only > "$policySource/.gitignore"
echo /nested/visible > "$policySource/nested/.nixignore"
mkdir -p "$policySource/nested/.git" "$policySource/nested/.hg" "$policySource/nested/.jj"
touch "$policySource/nested/.git/config" "$policySource/nested/.hg/requires" "$policySource/nested/.jj/repo"
policyStore=$(nix flake prefetch --refresh --json "path:$policySource" | jq -r .storePath)
test ! -e "$policyStore/git-only"
test -e "$policyStore/ignore-only"
test -e "$policyStore/nested/visible"
test ! -e "$policyStore/nested/.git"
test ! -e "$policyStore/nested/.hg"
test ! -e "$policyStore/nested/.jj"

echo /ignore-only > "$policySource/.ignore"
policyStore=$(nix flake prefetch --refresh --json "path:$policySource" | jq -r .storePath)
test -e "$policyStore/git-only"
test ! -e "$policyStore/ignore-only"

echo /nix-only > "$policySource/.nixignore"
policyStore=$(nix flake prefetch --refresh --json "path:$policySource" | jq -r .storePath)
test -e "$policyStore/git-only"
test -e "$policyStore/ignore-only"
test ! -e "$policyStore/nix-only"

# Compare the matcher against Git over a representative wildmatch corpus.
corpus=$TEST_ROOT/nixignore-wildmatch
createGitRepo "$corpus"
cat > "$corpus/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
cat > "$corpus/.gitignore" <<'EOF'
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
printf '%s\n' 'literal\ ' >> "$corpus/.gitignore"
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

# The root policy filters mounted submodule contents; the submodule's policy is inert.
subRepo=$TEST_ROOT/nixignore-submodule
createGitRepo "$subRepo"
echo ignored > "$subRepo/ignored"
echo kept > "$subRepo/kept"
echo /kept > "$subRepo/.nixignore"
git -C "$subRepo" add ignored kept .nixignore
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
test -e "$superSource/deps/sub/kept"
test -e "$superSource/deps/sub/.nixignore"

echo /deps/sub/ > "$superRepo/.nixignore"
git -C "$superRepo" add .nixignore
git -C "$superRepo" commit -m skip-submodule
superSource=$(nix flake prefetch --refresh --json \
    "git+file://$superRepo?ref=master&submodules=1" | jq -r .storePath)
test ! -e "$superSource/deps/sub"

# Final lock entries without a marker preserve the legacy source tree until updated.
legacyDep=$TEST_ROOT/nixignore-legacy-dep
createGitRepo "$legacyDep"
mkdir -p "$legacyDep/secret"
cat > "$legacyDep/flake.nix" <<'EOF'
{ outputs = _: {}; }
EOF
echo /secret/ > "$legacyDep/.nixignore"
echo legacy > "$legacyDep/secret/value"
git -C "$legacyDep" add flake.nix .nixignore secret/value
git -C "$legacyDep" commit -m initial
legacyDepAttrs=$(nix eval --impure --json --expr \
    "let tree = builtins.fetchTree {
      type = \"git\";
      url = \"file://$legacyDep\";
      ref = \"master\";
    }; in {
      lastModified = tree.lastModified;
      narHash = tree.narHash;
    }")

consumer=$TEST_ROOT/nixignore-consumer
mkdir -p "$consumer"
cat > "$consumer/flake.nix" <<EOF
{
  inputs.dep.url = "git+file://$legacyDep?ref=master";
  outputs = { dep, ... }: {
    hasSecret = builtins.pathExists "\${dep}/secret/value";
  };
}
EOF
nix flake lock "$consumer"
jq \
    --arg narHash "$(jq -r .narHash <<< "$legacyDepAttrs")" \
    --argjson lastModified "$(jq -r .lastModified <<< "$legacyDepAttrs")" \
    '.nodes.dep.locked |= (del(.nixIgnore) | .narHash = $narHash | .lastModified = $lastModified)' \
    "$consumer/flake.lock" > "$consumer/flake.lock.legacy"
mv "$consumer/flake.lock.legacy" "$consumer/flake.lock"
cp "$consumer/flake.lock" "$TEST_ROOT/nixignore-legacy.lock"
nix flake lock "$consumer"
cmp "$consumer/flake.lock" "$TEST_ROOT/nixignore-legacy.lock"
[[ $(nix eval --json "$consumer#hasSecret") = true ]]

nix flake update dep --flake "$consumer"
[[ $(jq -r .nodes.dep.locked.nixIgnore "$consumer/flake.lock") = 1 ]]
[[ $(nix eval --json "$consumer#hasSecret") = false ]]

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

# The source-selection version is recorded only when the policy actually
# changed the selection, so lock entries stay readable by a Nix that does not
# know the attribute.
noPolicy=$TEST_ROOT/nixignore-no-policy
mkdir -p "$noPolicy"
echo '{ outputs = _: {}; }' > "$noPolicy/flake.nix"
[[ $(nix flake metadata --json "path:$noPolicy" | jq -r '.locked.nixIgnore // "none"') = none ]]

noPolicyRepo=$TEST_ROOT/nixignore-no-policy-git
createGitRepo "$noPolicyRepo"
echo '{ outputs = _: {}; }' > "$noPolicyRepo/flake.nix"
echo /build > "$noPolicyRepo/.gitignore"
git -C "$noPolicyRepo" add flake.nix .gitignore
git -C "$noPolicyRepo" commit -m initial
[[ $(nix flake metadata --refresh --json "git+file://$noPolicyRepo?ref=master" | jq -r '.locked.nixIgnore // "none"') = none ]]

echo /build > "$noPolicyRepo/.nixignore"
git -C "$noPolicyRepo" add .nixignore
git -C "$noPolicyRepo" commit -m policy
[[ $(nix flake metadata --refresh --json "git+file://$noPolicyRepo?ref=master" | jq -r .locked.nixIgnore) = 1 ]]
