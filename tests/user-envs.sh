source common.sh

clearProfiles

set -x

# Query installed: should be empty.
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 0

# Query available: should contain several.
test "$(nix-env -p $profiles/test -f ./user-envs.nix -qa '*' | wc -l)" -eq 6

# Query descriptions.
nix-env -p $profiles/test -f ./user-envs.nix -qa '*' --description | grep silly

# Install "foo-1.0".
nix-env -p $profiles/test -f ./user-envs.nix -i foo-1.0

# Query installed: should contain foo-1.0 now (which should be
# executable).
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 1
nix-env -p $profiles/test -q '*' | grep -q foo-1.0
test "$($profiles/test/bin/foo)" = "foo-1.0"

# Disable foo.
nix-env -p $profiles/test --set-flag active false foo
! [ -e "$profiles/test/bin/foo" ]

# Enable foo.
nix-env -p $profiles/test --set-flag active true foo
[ -e "$profiles/test/bin/foo" ]

# Store the path of foo-1.0.
outPath10=$(nix-env -p $profiles/test -q --out-path --no-name '*' | grep foo-1.0)
echo "foo-1.0 = $outPath10"
test -n "$outPath10"

# Install "foo-2.0pre1": should remove foo-1.0.
nix-env -p $profiles/test -f ./user-envs.nix -i foo-2.0pre1

# Query installed: should contain foo-2.0pre1 now.
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 1
nix-env -p $profiles/test -q '*' | grep -q foo-2.0pre1
test "$($profiles/test/bin/foo)" = "foo-2.0pre1"

# Upgrade "foo": should install foo-2.0.
NIX_PATH=nixpkgs=./user-envs.nix:$NIX_PATH nix-env -p $profiles/test -f '<nixpkgs>' -u foo

# Query installed: should contain foo-2.0 now.
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 1
nix-env -p $profiles/test -q '*' | grep -q foo-2.0
test "$($profiles/test/bin/foo)" = "foo-2.0"

# Store the path of foo-2.0.
outPath20=$(nix-env -p $profiles/test -q --out-path --no-name '*' | grep foo-2.0)
test -n "$outPath20"

# Install bar-0.1, uninstall foo.
nix-env -p $profiles/test -f ./user-envs.nix -i bar-0.1
nix-env -p $profiles/test -f ./user-envs.nix -e foo

# Query installed: should only contain bar-0.1 now.
if nix-env -p $profiles/test -q '*' | grep -q foo; then false; fi
nix-env -p $profiles/test -q '*' | grep -q bar

# Rollback: should bring "foo" back.
nix-env -p $profiles/test --rollback
nix-env -p $profiles/test -q '*' | grep -q foo-2.0
nix-env -p $profiles/test -q '*' | grep -q bar

# Rollback again: should remove "bar".
nix-env -p $profiles/test --rollback
nix-env -p $profiles/test -q '*' | grep -q foo-2.0
if nix-env -p $profiles/test -q '*' | grep -q bar; then false; fi

# Count generations.
nix-env -p $profiles/test --list-generations
test "$(nix-env -p $profiles/test --list-generations | wc -l)" -eq 7

# Install foo-1.0, now using its store path.
echo $outPath10
nix-env -p $profiles/test -i "$outPath10"
nix-env -p $profiles/test -q '*' | grep -q foo-1.0

# Uninstall foo-1.0, using a symlink to its store path.
ln -sfn $outPath10/bin/foo $TEST_ROOT/symlink
nix-env -p $profiles/test -e $TEST_ROOT/symlink
if nix-env -p $profiles/test -q '*' | grep -q foo; then false; fi

# Install foo-1.0, now using a symlink to its store path.
nix-env -p $profiles/test -i $TEST_ROOT/symlink
nix-env -p $profiles/test -q '*' | grep -q foo

# Delete all old generations.
nix-env -p $profiles/test --delete-generations old

# Run the garbage collector.  This should get rid of foo-2.0 but not
# foo-1.0.
nix-collect-garbage
test -e "$outPath10"
! [ -e "$outPath20" ]

# Uninstall everything
nix-env -p $profiles/test -f ./user-envs.nix -e '*'
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 0

# Installing "foo" should only install the newest foo.
nix-env -p $profiles/test -f ./user-envs.nix -i foo
test "$(nix-env -p $profiles/test -q '*' | grep foo- | wc -l)" -eq 1
nix-env -p $profiles/test -q '*' | grep -q foo-2.0

# On the other hand, this should install both (and should fail due to
# a collision).
nix-env -p $profiles/test -f ./user-envs.nix -e '*'
! nix-env -p $profiles/test -f ./user-envs.nix -i foo-1.0 foo-2.0

# Installing "*" should install one foo and one bar.
nix-env -p $profiles/test -f ./user-envs.nix -e '*'
nix-env -p $profiles/test -f ./user-envs.nix -i '*'
test "$(nix-env -p $profiles/test -q '*' | wc -l)" -eq 2
nix-env -p $profiles/test -q '*' | grep -q foo-2.0
nix-env -p $profiles/test -q '*' | grep -q bar-0.1.1

# Test priorities: foo-0.1 has a lower priority than foo-1.0, so it
# should be possible to install both without a collision.  Also test
# ‘--set-flag priority’ to manually override the declared priorities.
nix-env -p $profiles/test -f ./user-envs.nix -e '*'
nix-env -p $profiles/test -f ./user-envs.nix -i foo-0.1 foo-1.0
[ "$($profiles/test/bin/foo)" = "foo-1.0" ]
nix-env -p $profiles/test -f ./user-envs.nix --set-flag priority 1 foo-0.1
[ "$($profiles/test/bin/foo)" = "foo-0.1" ]
