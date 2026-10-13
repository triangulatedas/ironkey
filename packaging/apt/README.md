# Self-hosted APT repository (repo.trisec.no)

Serves the `ironkey` / `ironkey-gui` `.deb`s so users can
`apt install ironkey`. Runs as a small Docker stack behind the existing
`trisec.no` Cloudflare tunnel (the one in `./dawn`), on a dedicated external
network `aptnet`. It runs **no tunnel of its own**.

- `apt` — nginx, serves the reprepro repo on `aptnet`.
- `updater` — pulls the **latest public GitHub release** `.deb`s (no auth),
  rebuilds + signs the repo with reprepro, and loops every 6h. It **generates the
  signing key on first run** into `./.gnupg-signing/` (never web-served).

## Deploy on the server (`~/docker/apt`)

```sh
# 1. Copy this directory's contents to ~/docker/apt on the server.

# 2. Create the shared network the cloudflared will also join:
docker network create aptnet

# 3. Attach the dawn cloudflared to aptnet so it can reach the apt container.
#    In ~/docker/dawn/docker-compose.yml, add aptnet to the cloudflared service
#    and declare it external:
#
#      services:
#        cloudflared:
#          networks:
#            - dawn_net
#            - stocknet
#            - aptnet        # <-- add
#      networks:
#        aptnet:             # <-- add
#          name: aptnet
#          external: true
#
#    then recreate just cloudflared:
#      cd ~/docker/dawn && docker compose up -d cloudflared

# 4. Bring up the apt stack (builds the updater image):
cd ~/docker/apt && docker compose up -d --build

# 5. Add a Cloudflare Public Hostname (Zero Trust dashboard, token tunnel):
#      repo.trisec.no  ->  http://ironkey-apt:80

# 6. IMPORTANT — stop Cloudflare caching the repo, or apt will hit
#    "File has unexpected size" after each publish (a cached-but-stale index).
#    Caching -> Cache Rules -> Create:
#      If  hostname equals repo.trisec.no  ->  Cache eligibility: Bypass cache
#    (nginx also marks dists/ no-store, but the CDN rule is the reliable fix.)
```

After changing `nginx.conf`, reload it: `docker compose restart apt`.

The updater builds the repo within a minute of first start (watch it with
`docker compose logs -f updater`). Verify from the server:
`curl -fsSL http://localhost  # via the container` or, once the hostname is live,
`curl -fsSL https://repo.trisec.no/dists/stable/Release`.

## What users run

```sh
curl -fsSL https://repo.trisec.no/ironkey-archive-keyring.gpg \
  | sudo tee /usr/share/keyrings/ironkey.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/ironkey.gpg] https://repo.trisec.no stable main" \
  | sudo tee /etc/apt/sources.list.d/ironkey.list
sudo apt update && sudo apt install ironkey      # desktop: pulls ironkey-gui via Recommends
# console-only: sudo apt install --no-install-recommends ironkey
```

## Operating it

- **New release**: nothing to do — the updater picks it up within `INTERVAL`
  (6h). To apply immediately: `docker compose restart updater`, or run once:
  `docker compose run --rm -e ONESHOT=1 updater`.
- **Back up the signing key**: `./.gnupg-signing/` is the repo's identity. If it
  is lost, every client must re-import a new key. Keep a copy somewhere safe.
- The published `Release` is signed; clients verify it via the keyring above.
