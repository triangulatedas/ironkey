FROM debian:stable-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      reprepro curl ca-certificates gnupg jq \
 && rm -rf /var/lib/apt/lists/*

COPY update.sh /usr/local/bin/update.sh
RUN chmod +x /usr/local/bin/update.sh

ENTRYPOINT ["/usr/local/bin/update.sh"]
