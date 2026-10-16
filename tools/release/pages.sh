#!/usr/bin/env bash
# Build the public site and push it to the public Pages repository.
#
#   tools/release/pages.sh              build only, output stays in build/pages
#   tools/release/pages.sh --preview    push to the `preview` branch (no deploy)
#   tools/release/pages.sh --publish    push to `main` (the live site)
#
# Preview never touches `main`; the live site only changes on --publish, which
# the release workflow runs after a tag ships.
set -euo pipefail
mode=build
case "${1:-}" in
 --preview) mode=preview ;;
 --publish) mode=publish ;;
 "") ;;
 *) echo "usage: $0 [--preview|--publish]" >&2; exit 2 ;;
esac

python3 -m pip install --quiet Markdown==3.7
python3 site/build.py build/pages
npm --prefix platform/frontend ci --ignore-scripts
VITE_DEMO=1 npm --prefix platform/frontend run build
mkdir -p build/pages/demos/platform
cp -a platform/frontend/dist/. build/pages/demos/platform/

if [[ "$mode" == build ]]; then
 echo "Built build/pages (serve it locally, or pass --preview/--publish to push)."
 exit 0
fi

branch=$([[ "$mode" == preview ]] && echo preview || echo main)
target=$(mktemp -d)
trap 'rm -rf "$target"' EXIT
if [[ -n "${GH_TOKEN:-}" ]]; then gh auth setup-git; fi
gh repo clone FUjr/phicomm-community-pages "$target/public" -- --depth=1
# Only generated public output may cross the private/public boundary.
find "$target/public" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf -- {} +
cp -a build/pages/. "$target/public/"
git -C "$target/public" checkout -B "$branch"
git -C "$target/public" add .
if ! git -C "$target/public" diff --cached --quiet; then
 git -C "$target/public" -c user.name='Phicomm release' -c user.email='release@users.noreply.github.com' \
  commit -m "Publish site from ${GITHUB_SHA:-$(git rev-parse HEAD)}"
 git -C "$target/public" push --force-with-lease origin "HEAD:${branch}"
 echo "Pushed ${branch}."
else
 echo "No site changes to publish."
fi
