#!/usr/bin/env bash
# Prepare archive -> verify/compile notes -> generate site -> final consistency.
# No argument publishes because the tag workflow invokes this script bare.
set -euo pipefail
mode=publish
case "${1:-}" in
 --preview) mode=preview ;;
 --build-only) mode=build ;;
 "") ;;
 *) echo "usage: $0 [--preview|--build-only]" >&2; exit 2 ;;
esac
python3 -m pip install --quiet Markdown==3.7 cryptography==46.0.3
: "${PHICOMM_PAGES_REPO:=phicomm-io/phicomm-io.github.io}"
: "${PHICOMM_PAGES_BRANCH:=main}"
target=$(mktemp -d)
trap 'rm -rf "$target"' EXIT
if [[ -n "${GH_TOKEN:-}" ]]; then gh auth setup-git; fi
# A fixed snapshot supports offline local builds. Publication always uses actual
# retained files and cryptographically verifies every firmware identity.
if [[ "$mode" == build && -z "${PHICOMM_PAGES_ARCHIVE:-}" ]]; then
 python3 site/build.py build/pages
 echo "Built build/pages from the fixed historical metadata snapshot."
 exit 0
fi
if [[ "$mode" == build ]]; then
 cp -a "$PHICOMM_PAGES_ARCHIVE/." "$target/archive"
else
 gh repo clone "$PHICOMM_PAGES_REPO" "$target/public" -- --depth=1
 mkdir -p "$target/archive"
 for entry in releases ota; do
  if [[ -d "$target/public/$entry" ]]; then cp -a "$target/public/$entry" "$target/archive/"; fi
 done
 # Refresh only this temporary archive. A failed refresh cannot leave partially
 # updated metadata or mutate retained originals used by the final build.
 cp -a "$target/archive" "$target/refresh"
 if python3 tools/release/archive_pages.py "$target/refresh"; then
  cp -a "$target/refresh/." "$target/archive/"
 elif [[ ! -s "$target/archive/releases/index.json" ]]; then
  echo "No verified static archive is available." >&2; exit 1
 else
  echo "Keeping retained archive; upstream refresh failed." >&2
 fi
fi
python3 tools/release/archive_pages.py "$target/archive" --existing
python3 tests/test_site_docs.py
python3 site/build.py build/pages --archive "$target/archive" --formal
if [[ "$mode" == build ]]; then
 echo "Built and verified build/pages against actual archived firmware."
 exit 0
fi
for entry in .github tools; do
 if [[ -d "$target/public/$entry" ]]; then cp -a "$target/public/$entry" build/pages/; fi
done
# Pages builds IotCloud from its own lock. Publish the same verified frontend
# as these guides and device demos instead of retaining an older platform UI.
if [[ -d build/pages/tools ]]; then
 cp frontend.lock.json build/pages/tools/iotcloud.lock.json
 cp tools/frontend_release.py build/pages/tools/frontend_release.py
fi
if [[ -f "$target/public/demos/platform/release.json" ]]; then
 rm -rf build/pages/demos/platform
 cp -a "$target/public/demos/platform" build/pages/demos/
fi
python3 tools/release/user_notes.py --verify-output build/pages --formal
# The tag workflow supplies the existing OTA credential. Preview and local
# builds never mutate mirrors; public data is uploaded before its pointer.
if [[ "$mode" == publish && -n "${CLOUDFLARE_API_TOKEN:-}" ]]; then
 python3 tools/release/publish_user_notes.py build/pages
fi
# Only generated public files cross the repository boundary. Immutable resources
# are included before their index in the same atomic Git commit.
find "$target/public" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf -- {} +
cp -a build/pages/. "$target/public/"
branch="$PHICOMM_PAGES_BRANCH"
if [[ "$mode" == preview ]]; then branch=preview; fi
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
