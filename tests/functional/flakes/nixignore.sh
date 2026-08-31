#!/usr/bin/env bash

source ./common.sh

requireGit

repo=$TEST_ROOT/nixignore-repo
createGitRepo "$repo"
mkdir -p "$repo/secret" "$repo/sub"
git -C "$repo" config filter.nixignore.clean "sed 's/private/encrypted/g'"
git -C "$repo" config filter.nixignore.smudge "sed 's/encrypted/private/g'"

cat > "$repo/flake.nix" <<'EOF'
{
  outputs = { self }: {
    visible = builtins.readFile ./visible;
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
echo '/secret/value filter=nixignore' > "$repo/.gitattributes"
git -C "$repo" add flake.nix sub/flake.nix visible secret/value .nixignore .gitattributes
git -C "$repo" commit -m initial
[[ $(git -C "$repo" show HEAD:secret/value) = encrypted ]]
[[ $(< "$repo/secret/value") = private ]]

nix flake prefetch --out-link "$TEST_ROOT/result-local" "$repo"
source1=$(readlink "$TEST_ROOT/result-local")
test -e "$source1/flake.nix"
test -e "$source1/.nixignore"
test ! -e "$source1/secret"
[[ $(nix eval --raw "$repo#visible") = public ]]
[[ $(nix flake metadata --json "$repo" | jq -r .locked.nixIgnore) = 1 ]]

remoteSource=$(nix flake prefetch --json --out-link "$TEST_ROOT/result-remote" "git+file://$repo?ref=master" | jq -r .storePath)
test ! -e "$remoteSource/secret"
[[ $source1 = "$remoteSource" ]]

fingerprint1=$(nix flake metadata --json "$repo" | jq -r .fingerprint)
echo changed-private > "$repo/secret/value"
fingerprint2=$(nix flake metadata --refresh --json "$repo" | jq -r .fingerprint)
[[ $fingerprint1 = "$fingerprint2" ]]
[[ $(nix flake metadata --json "$repo" | jq -r '.dirtyRevision // "clean"') = clean ]]

echo '# policy change' >> "$repo/.nixignore"
policyFingerprint=$(nix flake metadata --refresh --json "$repo" | jq -r .fingerprint)
[[ $fingerprint1 != "$policyFingerprint" ]]

echo changed-public > "$repo/visible"
fingerprint3=$(nix flake metadata --refresh --json "$repo" | jq -r .fingerprint)
[[ $policyFingerprint != "$fingerprint3" ]]

# `dir=` does not move the ignore root into the selected flake directory.
nix flake prefetch --out-link "$TEST_ROOT/result-sub" "git+file://$repo?dir=sub"
subSource=$(readlink "$TEST_ROOT/result-sub")
test ! -e "$subSource/secret"

pathSource=$TEST_ROOT/nixignore-path
mkdir -p "$pathSource/secret"
cp "$repo/flake.nix" "$pathSource/flake.nix"
echo public > "$pathSource/visible"
echo private > "$pathSource/secret/value"
echo /secret/ > "$pathSource/.nixignore"
nix flake prefetch --out-link "$TEST_ROOT/result-path" "path:$pathSource"
realisedPath=$(readlink "$TEST_ROOT/result-path")
test ! -e "$realisedPath/secret"
