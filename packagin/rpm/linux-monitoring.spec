Name: linux-monitoring
Version: 1.0.0
Release: 1%{?dist}
Summary: Standalone Linux monitoring dashboard for remote VM service checks
License: MIT
URL: https://example.com/monitoring-linux
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake >= 3.16
BuildRequires: gcc-c++
BuildRequires: sqlite-devel

%description
MonitoringLinux installs a small local monitoring web application that can connect via SSH to remote Linux machines in the same LAN and monitor configured services from a single dashboard.

The application is intended to be launched manually from the command line and does not enable automatic startup.

%prep
%autosetup

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

%install
rm -rf %{buildroot}
cmake --install build --prefix %{_prefix}
install -d %{buildroot}%{_sysconfdir}/monitoring-linux
install -m 0644 config/monitoring.conf %{buildroot}%{_sysconfdir}/monitoring-linux/monitoring.conf

%files
%{_sbindir}/monitoring-linux
%config(noreplace) %{_sysconfdir}/monitoring-linux/monitoring.conf
%{_datadir}/monitoring-linux

%changelog
* Tue Oct 13 2026 - Initial RPM release
- Initial standalone remote monitoring package for Linux hosts
- Manual launch only; no automatic service activation

