#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="$(mktemp -d -p /tmp katmed-image-cleanup-build-XXXXXX)"
trap 'rm -rf -- "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$repo_root/firmware/components/katmed_images" \
  "$repo_root/firmware/components/katmed_images/katmed_image_store_cleanup.c" \
  "$repo_root/tests/host_image_store/check_orphan_cleanup.c" -o "$out/check"
"$out/check"
cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/firmware/components/katmed_images" \
  -I"$repo_root/firmware/managed_components/joltwallet__littlefs/src/littlefs" \
  "$repo_root/firmware/components/katmed_images/katmed_image_store_cleanup.c" \
  "$repo_root/firmware/managed_components/joltwallet__littlefs/src/littlefs/lfs.c" \
  "$repo_root/firmware/managed_components/joltwallet__littlefs/src/littlefs/lfs_util.c" \
  "$repo_root/tests/host_image_store/check_littlefs_orphan_remount.c" \
  -o "$out/check_littlefs"
"$out/check_littlefs"
