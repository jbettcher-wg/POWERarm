#!/bin/bash
# SPDX-License-Identifier: MIT
# Extract an OCI/Docker registry image into a plain rootfs directory.  No Docker, no root.
#
#   oci-extract.sh [options] <image[:tag|@sha256:digest]> <dest>
#
#   --platform OS/ARCH[/VARIANT]   manifest-list selection (default linux/arm64)
#   --cache DIR       blob cache (default ${XDG_CACHE_HOME:-~/.cache}/powerarm/oci)
#   --record FILE     image record (default <dest>.oci-manifest)
#   --force           replace a non-empty <dest>
#
# Examples:
#   oci-extract.sh debian:trixie-slim ~/.local/share/powerarm/RootFS/debian-trixie
#   oci-extract.sh docker.io/library/debian@sha256:<index digest> <dest>
#   oci-extract.sh ghcr.io/owner/image:tag <dest>
#
# Every manifest and blob is checked against its sha256 digest; layers are applied in
# order with .wh.<name> whiteouts and .wh..wh..opq opaque directories.  Ownership is not
# preserved and device nodes are skipped (both need root).  The record file holds the
# resolved index/manifest/layer digests, a pinned image reference and the tree's content hash.
set -euo pipefail
case "${1:-}" in -h|--help|"") sed -n '3,21p' "$0"; exit 2 ;; esac
exec python3 "$(cd "$(dirname "$0")" && pwd)/oci_extract.py" "$@"
