#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

cd "${PROJECT_ROOT}"

mkdir -p "${HOME}/rpmbuild/SOURCES" "${HOME}/rpmbuild/SPECS"

TAR_FILE="${HOME}/rpmbuild/SOURCES/linux-monitoring-1.0.0.tar.gz"
if [ -f "${TAR_FILE}" ]; then
  rm -f "${TAR_FILE}"
fi

TMP_DIR="$(mktemp -d)"
SRC_DIR="${TMP_DIR}/linux-monitoring-1.0.0"
mkdir -p "${SRC_DIR}"

tar -C "${PROJECT_ROOT}" \
  --exclude='.git' \
  --exclude='build' \
  --exclude='*.swp' \
  -czf - . | tar -C "${SRC_DIR}" -xzf -

tar -C "${TMP_DIR}" -czf "${TAR_FILE}" linux-monitoring-1.0.0
rm -rf "${TMP_DIR}"

cp "${SCRIPT_DIR}/linux-monitoring.spec" "${HOME}/rpmbuild/SPECS/"

rpmbuild -bb "${HOME}/rpmbuild/SPECS/linux-monitoring.spec"

echo "RPM build completed. Check: ${HOME}/rpmbuild/RPMS/x86_64/"
