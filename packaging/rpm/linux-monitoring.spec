Name: linux-monitoring
Version: 1.0.0
Release: 4%{?dist}
Summary: Standalone Linux monitoring dashboard for remote VM service checks
License: MIT
URL: https://example.com/monitoring-linux
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake >= 3.16
BuildRequires: gcc-c++

%description
MonitoringLinux installs a small local monitoring web application that can connect via SSH to remote Linux machines in the same LAN and monitor configured services from a single dashboard.

The application is intended to be launched manually from the command line and does not enable automatic startup.

%prep
%autosetup

%build
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/opt -DCMAKE_INSTALL_SYSCONFDIR=%{_sysconfdir}
cmake --build build

%install
rm -rf %{buildroot}
DESTDIR=%{buildroot} cmake --install build --prefix /opt
install -d %{buildroot}%{_bindir}
ln -s /opt/monitoring-linux/monitoring-linux %{buildroot}%{_bindir}/monitoring-linux

%files
%{_bindir}/monitoring-linux
%dir /opt/monitoring-linux
/opt/monitoring-linux/monitoring-linux
/opt/monitoring-linux/monitoring-linux-ssh-askpass
/opt/monitoring-linux/web
%config(noreplace) %{_sysconfdir}/monitoring-linux/monitoring.conf

%changelog
* Tue Oct 13 2026 - Release 4
- Add named target management and optional in-memory SSH password prompts
- Restrict the web dashboard to local access
* Tue Oct 13 2026 - Release 3
- Embed SQLite in the application; no SQLite runtime package is required
- Build RPM in /opt/monitoring-linux/rpmbuild
* Tue Oct 13 2026 - Initial RPM release
- Initial standalone remote monitoring package for Linux hosts
- Manual launch only; no automatic service activation

