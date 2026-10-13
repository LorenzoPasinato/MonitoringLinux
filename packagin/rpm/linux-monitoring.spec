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
Requires: sqlite
Requires: openssh-clients

%description
MonitoringLinux installs a small local monitoring web application that can connect via SSH to remote Linux machines in the same LAN and monitor configured services from a single dashboard.

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
install -d %{buildroot}%{_unitdir}
install -m 0644 packagin/rpm/linux-monitoring.service %{buildroot}%{_unitdir}/linux-monitoring.service

%files
%{_sbindir}/monitoring-linux
%config(noreplace) %{_sysconfdir}/monitoring-linux/monitoring.conf
%{_datadir}/monitoring-linux
%{_unitdir}/linux-monitoring.service

%post
%systemd_post linux-monitoring.service

%preun
%systemd_preun linux-monitoring.service

%postun
%systemd_postun_with_restart linux-monitoring.service

%changelog
* Tue Oct 13 2026 - Initial RPM release
- Initial standalone remote monitoring package for Linux hosts

