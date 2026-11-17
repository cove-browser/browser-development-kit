#!/bin/sh
# Runs actionlint when workflows exist (actionlint fails when .github/workflows is absent).
set -eu
if [ -d .github/workflows ] && ls .github/workflows/*.yml >/dev/null 2>&1; then
  actionlint
else
  echo "actionlint: no workflows yet"
fi
