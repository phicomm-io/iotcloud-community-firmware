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

: "${PHICOMM_PAGES_REPO:=phicomm-io/phicomm-io.github.io}"
: "${PHICOMM_PAGES_BRANCH:=main}"
python3 -m pip install --quiet Markdown==3.7
# Verify published guide URLs and homepage priorities before replacing the site.
python3 tests/test_site_docs.py
python3 site/build.py build/pages

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
# Preserve the static archive even when the external mirrors are retired.
if [[ -d "$target/public/releases" ]]; then
 cp -a "$target/public/releases" build/pages/
fi
python3 tools/release/archive_pages.py build/pages || {
 if [[ ! -s build/pages/releases/index.json ]]; then
  echo "No verified static archive is available." >&2
  exit 1
 fi
 echo "Keeping the existing GitHub archive; upstream refresh failed." >&2
}
# The retained archive must still provide the small device manifests when an
# upstream refresh fails; otherwise a site-only publish removes OTA discovery.
python3 tools/release/archive_pages.py build/pages --existing
# Keep the public site's independent MQTT release automation and platform demo.
for entry in .github tools; do
 if [[ -d "$target/public/$entry" ]]; then
  cp -a "$target/public/$entry" build/pages/
 fi
done
if [[ -f "$target/public/demos/platform/release.json" ]]; then
 rm -rf build/pages/demos/platform
 cp -a "$target/public/demos/platform" build/pages/demos/
fi
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
