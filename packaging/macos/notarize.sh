#!/usr/bin/env bash
# UNVERIFIED: requires Apple notarytool credentials and network access to
# Apple's notary service. Placeholder profile-based flow; no-ops locally.
#
# Submits a DMG (or app) for notarization and staples the ticket so Gatekeeper
# accepts it offline (build-and-packaging spec: "signed/notarized DMG").
set -euo pipefail

ARTIFACT="${1:?usage: notarize.sh <path-to-dmg-or-app>}"
NOTARY_PROFILE="${MACOS_NOTARY_KEYCHAIN_PROFILE:?MACOS_NOTARY_KEYCHAIN_PROFILE not set}"

echo "==> Submitting ${ARTIFACT} for notarization"
xcrun notarytool submit "${ARTIFACT}" \
    --keychain-profile "${NOTARY_PROFILE}" \
    --wait

echo "==> Stapling notarization ticket"
xcrun stapler staple "${ARTIFACT}"
xcrun stapler validate "${ARTIFACT}"
