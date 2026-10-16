#!/usr/bin/env bash
# Build the public site and push it to the public Pages repository.
#
#   tools/release/pages.sh                build and publish to `main` (live site)
#   tools/release/pages.sh --preview      build and push the `preview` branch
#   tools/release/pages.sh --build-only   build only, output stays in build/pages
#
# No argument publishes, because the release workflow calls this script bare
# after a tag ships. Preview never touches the publish branch.
#
# Target repository and branch are configurable so the site can move to a
# user/organization root domain without editing this script:
#
#   PHICOMM_PAGES_REPO=phicomm-io/phicomm-io.github.io \
#   PHICOMM_PAGES_BRANCH=main tools/release/pages.sh
#
# A <owner>/<owner>.github.io repository serves at https://<owner>.github.io/.
set -euo pipefail
mode=publish
case "${1:-}" in
 --preview) mode=preview ;;
 --build-only) mode=build ;;
 "") ;;
 *) echo "usage: $0 [--preview|--build-only]" >&2; exit 2 ;;
esac

: "${PHICOMM_PAGES_REPO:=FUjr/phicomm-community-pages}"
: "${PHICOMM_PAGES_BRANCH:=main}"

python3 -m pip install --quiet Markdown==3.7
python3 site/build.py build/pages
npm --prefix platform/frontend ci --ignore-scripts
VITE_DEMO=1 npm --prefix platform/frontend run build
mkdir -p build/pages/demos/platform
cp -a platform/frontend/dist/. build/pages/demos/platform/

if [[ "$mode" == build ]]; then
 echo "Built build/pages (serve it locally, or run without --build-only to push)."
 exit 0
fi

if [[ "$mode" == preview ]]; then
 branch=preview
else
 branch="$PHICOMM_PAGES_BRANCH"
fi
target=$(mktemp -d)
trap 'rm -rf "$target"' EXIT
if [[ -n "${GH_TOKEN:-}" ]]; then gh auth setup-git; fi
gh repo clone "$PHICOMM_PAGES_REPO" "$target/public" -- --depth=1
# Only generated public output may cross the private/public boundary.
find "$target/public" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf -- {} +
cp -a build/pages/. "$target/public/"
git -C "$target/public" checkout -B "$branch"
git -C "$target/public" add .
if ! git -C "$target/public" diff --cached --quiet; then
 git -C "$target/public" -c user.name='Phicomm release' -c user.email='release@users.noreply.github.com' \
  commit -m "Publish site from ${GITHUB_SHA:-$(git rev-parse HEAD)}"
 git -C "$target/public" push --force-with-lease origin "HEAD:${branch}"
 echo "Pushed ${branch} to ${PHICOMM_PAGES_REPO}."
else
 echo "No site changes to publish."
fi
