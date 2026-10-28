#!/bin/sh
# Publishes main (and tags) to a public remote — from a COPY whose history is cleaned first.
# The original repository is never rewritten.
#
#   sh scripts/publish.sh https://github.com/<you>/Platinenlupe.git
#
# 1. clones this repository into a temporary folder
# 2. rewrites every author/committer that is not a *noreply* address to PL_PUBLIC_NAME <PL_PUBLIC_EMAIL>
#    (default: user.name / user.email of this repository) — merge commits made by a self-hosted forge
#    otherwise carry the account's real address
# 3. runs scripts/privacy_check.py over the whole cleaned history and stops on any finding
# 4. pushes main and the tags to the target (set PL_DRY_RUN=1 to stop before the push)
set -eu
target="${1:?usage: publish.sh <target-remote-url>}"
src=$(git rev-parse --show-toplevel)
name="${PL_PUBLIC_NAME:-$(git -C "$src" config user.name)}"
email="${PL_PUBLIC_EMAIL:-$(git -C "$src" config user.email)}"
case "$email" in *noreply*) ;; *) echo "PL_PUBLIC_EMAIL should be a noreply address, got: $email" >&2; exit 2;; esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git clone -q --branch main "$src" "$work/repo"
cd "$work/repo"
git fetch -q --tags "$src"
git remote remove origin                      # only what gets published may remain in the clone
export PL_NAME="$name" PL_EMAIL="$email"
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch -f --env-filter '
case "$GIT_AUTHOR_EMAIL" in *noreply*) ;; *) GIT_AUTHOR_NAME="$PL_NAME"; GIT_AUTHOR_EMAIL="$PL_EMAIL";; esac
case "$GIT_COMMITTER_EMAIL" in *noreply*) ;; *) GIT_COMMITTER_NAME="$PL_NAME"; GIT_COMMITTER_EMAIL="$PL_EMAIL";; esac
export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
' --tag-name-filter cat -- main --tags >/dev/null
git for-each-ref --format='%(refname)' refs/original | xargs -r -n1 git update-ref -d
python3 "$src/scripts/privacy_check.py"     # the checker from the working copy, run on the cleaned clone
echo "history cleaned: $(git rev-list --count main) commits, all checks passed"
if [ "${PL_DRY_RUN:-0}" = 1 ]; then echo "dry run — not pushing to $target"; exit 0; fi
branch=main
git push "$target" "$branch" --tags
