#!/usr/bin/env bash

source ./common.sh

requireGit

repo=$TEST_ROOT/repo

createGitRepo "$repo"

cat > "$repo/flake.nix" <<EOF
{
  outputs = { ... }: {
    x = 1;
    y = assert false; 1;
    z = builtins.readFile ./foo;
    a = import ./foo;
    b = import ./dir;
  };
}
EOF

[[ $(nix eval "$repo#x") = 1 ]]

git -C "$repo" add flake.nix

[[ $(nix eval "$repo#x") = 1 ]]

expectStderr 1 nix eval "$repo#y" | grepQuiet '/flake.nix:'

git -C "$repo" commit -a -m foo

expectStderr 1 nix eval "git+file://$repo?ref=master#y" | grepQuiet "at «git+file://$repo?ref=master&rev=.*»/flake.nix:"

expectStderr 1 nix eval "$repo#z" | grepQuiet "foo.*does not exist"
expectStderr 1 nix eval "git+file://$repo?ref=master#z" | grepQuiet "error: '«git+file://$repo?ref=master&rev=.*»/foo' does not exist"
expectStderr 1 nix eval "$repo#a" | grepQuiet "foo.*does not exist"

echo 123 > "$repo/foo"

[[ $(nix eval --raw "$repo#z") = 123 ]]
[[ $(nix eval "$repo#a") = 123 ]]
expectStderr 1 nix eval "git+file://$repo?ref=master#z" | grepQuiet "error: '«git+file://$repo?ref=master&rev=.*»/foo' does not exist"

expectStderr 1 nix eval "$repo#b" | grepQuiet "dir.*does not exist"

mkdir -p "$repo/dir"
echo 456 > "$repo/dir/default.nix"

[[ $(nix eval "$repo#b") = 456 ]]
expectStderr 1 nix eval "git+file://$repo?ref=master#b" | grepQuiet "dir.*does not exist"
