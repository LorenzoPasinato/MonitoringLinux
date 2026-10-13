#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

cd "${PROJECT_ROOT}"

if [[ "${EUID}" -eq 0 ]]; then
  echo "Run this script as the build user, not as root." >&2
  exit 1
fi

RPMBUILD_TOPDIR="/opt/monitoring-linux/rpmbuild"
if [[ ! -w "${RPMBUILD_TOPDIR}" ]]; then
  if [[ -d "${RPMBUILD_TOPDIR}" ]]; then
    sudo chown -R "$(id -u):$(id -g)" "${RPMBUILD_TOPDIR}"
  else
    sudo install -d -m 0755 -o "$(id -u)" -g "$(id -g)" "${RPMBUILD_TOPDIR}"
  fi
fi

mkdir -p "${RPMBUILD_TOPDIR}/SOURCES" "${RPMBUILD_TOPDIR}/SPECS"

TAR_FILE="${RPMBUILD_TOPDIR}/SOURCES/linux-monitoring-1.0.0.tar.gz"
if [ -f "${TAR_FILE}" ]; then
  rm -f "${TAR_FILE}"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT
SRC_DIR="${TMP_DIR}/linux-monitoring-1.0.0"
mkdir -p "${SRC_DIR}"

tar -C "${PROJECT_ROOT}" \
  --exclude='.git' \
  --exclude='build' \
  --exclude='*.swp' \
  -czf - . | tar -C "${SRC_DIR}" -xzf -

tar -C "${TMP_DIR}" -czf "${TAR_FILE}" linux-monitoring-1.0.0

cp "${SCRIPT_DIR}/linux-monitoring.spec" "${RPMBUILD_TOPDIR}/SPECS/"

rpmbuild --define "_topdir ${RPMBUILD_TOPDIR}" -bb "${RPMBUILD_TOPDIR}/SPECS/linux-monitoring.spec"

echo "RPM build completed. Check: ${RPMBUILD_TOPDIR}/RPMS/"
