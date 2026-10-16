#!/usr/bin/env bash
set -euo pipefail
python3 -m pip install Markdown==3.7
python3 site/build.py build/pages
npm --prefix platform/frontend ci --ignore-scripts
VITE_DEMO=1 npm --prefix platform/frontend run build
mkdir -p build/pages/demos/platform
cp -a platform/frontend/dist/. build/pages/demos/platform/
target=$(mktemp -d)
trap 'rm -rf "$target"' EXIT
if [[ -n "${GH_TOKEN:-}" ]]; then gh auth setup-git; fi
gh repo clone FUjr/phicomm-community-pages "$target/public" -- --depth=1
# Only generated public output may cross the private/public boundary.
find "$target/public" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf -- {} +
cp -a build/pages/. "$target/public/"
git -C "$target/public" add .
if ! git -C "$target/public" diff --cached --quiet; then
 git -C "$target/public" -c user.name='Phicomm release' -c user.email='release@users.noreply.github.com' commit -m "Publish site from ${GITHUB_SHA:-$(git rev-parse HEAD)}"
 git -C "$target/public" push origin HEAD:main
fi
