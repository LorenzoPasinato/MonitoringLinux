Name: linux-monitoring
Version: 1.0.0
Release: 15%{?dist}
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
* Tue Oct 13 2026 - Release 15
- Support multiple independent SSH terminal windows and named minimized sessions
* Tue Oct 13 2026 - Release 14
- Limit monitored machine layout to two columns and add movable/resizable terminal window
* Tue Oct 13 2026 - Release 13
- Use a borderless trash icon for Delete and show SSH connection state per machine
* Tue Oct 13 2026 - Release 12
- Move Edit/Delete to compact VM title icons and label machine poweroff as Shutdown
* Tue Oct 13 2026 - Release 11
- Add per-service restart/stop and machine reboot/poweroff controls
* Tue Oct 13 2026 - Release 10
- Add confirmed service and machine restart/stop controls
- Show SSH username, host and port under each machine name
* Tue Oct 13 2026 - Release 9
- Add interactive SSH terminals for monitored machines
- Vendor xterm.js for a full terminal emulator in the dashboard
* Tue Oct 13 2026 - Release 8
- Update minimalist dashboard styling and layout
* Tue Oct 13 2026 - Release 7
- Batch SSH service probes per machine and run checks with bounded concurrency
- Serve HTTP requests with a fixed worker pool and batch SQLite writes
* Tue Oct 13 2026 - Release 6
- Add SSH password to the machine form and group services per machine
* Tue Oct 13 2026 - Release 5
- Refresh dashboard data without overlapping requests
- Wake the monitor immediately after target or credential changes
* Tue Oct 13 2026 - Release 4
- Add named target management and optional in-memory SSH password prompts
- Restrict the web dashboard to local access
* Tue Oct 13 2026 - Release 3
- Embed SQLite in the application; no SQLite runtime package is required
- Build RPM in /opt/monitoring-linux/rpmbuild
* Tue Oct 13 2026 - Initial RPM release
- Initial standalone remote monitoring package for Linux hosts
- Manual launch only; no automatic service activation

