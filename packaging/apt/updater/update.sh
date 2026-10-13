#!/bin/sh
# Build/refresh the ironkey APT repo from the latest public GitHub release.
# - Generates an unattended signing key on first run (kept in $GNUPGHOME, which
#   is NOT under the web root, so the private key is never served).
# - Pulls the latest release's .deb assets over HTTPS (public repo, no auth).
# - reprepro includedeb + signs the Release.
# Loops every $INTERVAL seconds (set ONESHOT=1 to run once and exit).
set -e

REPO=/repo
export GNUPGHOME=/gnupg
REPO_SLUG=${REPO_SLUG:-triangulatedas/ironkey}
SUITE=${SUITE:-stable}
INTERVAL=${INTERVAL:-21600}

mkdir -p "$GNUPGHOME" && chmod 700 "$GNUPGHOME"
mkdir -p "$REPO/conf"

# 1. Signing key (generate once).
if ! gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec'; then
    echo "update: generating repo signing key..."
    cat > /tmp/kp <<EOF
%no-protection
Key-Type: RSA
Key-Length: 3072
Key-Usage: sign
Name-Real: IronKey APT (trisec.no)
Name-Email: apt@trisec.no
Expire-Date: 0
%commit
EOF
    gpg --batch --gen-key /tmp/kp
    rm -f /tmp/kp
fi
FPR=$(gpg --list-keys --with-colons | awk -F: '/^fpr:/{print $10; exit}')
echo "update: signing key $FPR"
gpg --export "$FPR" > "$REPO/ironkey-archive-keyring.gpg"

# 2. reprepro distribution config.
cat > "$REPO/conf/distributions" <<EOF
Origin: IronKey
Label: IronKey
Codename: $SUITE
Suite: $SUITE
Architectures: amd64 arm64
Components: main
Description: IronKey Locker+ G2 unlocker for Linux ($REPO_SLUG)
SignWith: $FPR
EOF

update_once() {
    tmp=$(mktemp -d)
    echo "update: fetching latest release of $REPO_SLUG ..."
    urls=$(curl -fsSL "https://api.github.com/repos/$REPO_SLUG/releases/latest" \
        | jq -r '.assets[].browser_download_url | select(endswith(".deb"))' || true)
    if [ -z "$urls" ]; then
        echo "update: no .deb assets found (yet)."
        rm -rf "$tmp"
        return 0
    fi
    for u in $urls; do
        echo "  - $u"
        (cd "$tmp" && curl -fsSLO "$u")
    done
    for d in "$tmp"/*.deb; do
        # includedeb refuses a version already present — fine on re-runs.
        reprepro -b "$REPO" includedeb "$SUITE" "$d" || true
    done
    rm -rf "$tmp"
    echo "update: repo now contains:"
    reprepro -b "$REPO" list "$SUITE" || true
}

update_once
[ "${ONESHOT:-0}" = "1" ] && exit 0
while :; do
    sleep "$INTERVAL"
    update_once
done
