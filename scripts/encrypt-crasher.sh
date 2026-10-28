#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# A crashing input the nightly fuzz run found is a reproducer for a
# vulnerability nobody has been told about, and a CI artifact on a public
# repository can be read by anyone signed in. So before it is uploaded it is
# encrypted here to the repository's public key (.github/fuzz-crasher.pub):
# a fresh AES key for the archive, that key sealed with RSA. Only the holder
# of the private key reads it.
#
#   scripts/encrypt-crasher.sh <dir> <out>      # <out>.tar.enc and <out>.key.enc
#
# To read one, with the private key (kept off the repository, Sam's):
#
#   openssl pkeyutl -decrypt -inkey fuzz-crasher.pem -in <out>.key.enc -out key.bin
#   openssl enc -d -aes-256-cbc -pbkdf2 -pass file:key.bin -in <out>.tar.enc | tar xz
set -euo pipefail

dir="$1"   # paths are the caller's, so no cd: the key is found from the script's own place
out="$2"
pub="$(cd "$(dirname "$0")/.." && pwd)/.github/fuzz-crasher.pub"

key="$(mktemp)"
trap 'rm -f "$key"' EXIT
openssl rand -hex 32 > "$key"
tar czf - -C "$(dirname "$dir")" "$(basename "$dir")" | openssl enc -aes-256-cbc -pbkdf2 -pass "file:$key" -out "$out.tar.enc"
openssl pkeyutl -encrypt -pubin -inkey "$pub" -in "$key" -out "$out.key.enc"
echo "encrypt-crasher: $(find "$dir" -type f | wc -l | tr -d ' ') file(s) sealed as $out.tar.enc + $out.key.enc"
