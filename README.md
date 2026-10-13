# MonitoringLinux

MonitoringLinux is a standalone Linux monitoring application designed to run on one control node and monitor other Linux machines in the same LAN using SSH. The project ships a local web dashboard and stores monitoring data in SQLite.

## Main goal

Install the RPM on a single Red Hat 9 / AlmaLinux machine, launch the binary from the command line, open the local web UI, and add the IPs and services of the remote Linux hosts you want to monitor.

## Features

- local web dashboard served on localhost
- SSH-based checks against remote Linux VMs
- named machines with editable connection details and removal
- optional SSH password prompt after key authentication fails
- monitoring of service names such as sshd, nginx, httpd, postgresql, etc.
- persistent configuration in SQLite
- no external web service required
- portable RPM package intended for LAN-only monitoring

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Web UI

Open the browser on the same machine:

```text
http://localhost:8080/
```

## SSH access requirement

The control machine must have the OpenSSH client and be able to connect to the target Linux hosts. SSH key authentication is tried first; when it is rejected, the dashboard asks for that machine's password.

Example:

```bash
ssh-copy-id root@10.0.0.22
```

## RPM packaging on Red Hat 9 / AlmaLinux

The RPM spec is available in `packaging/rpm/linux-monitoring.spec`.

This project is intentionally designed as a manual-start application:
- no automatic systemd activation
- no external package installation required beyond the standard OS base libraries
- installed from a standalone RPM and launched manually from the command line

Install the build prerequisites on the build machine:

```bash
sudo dnf install -y rpm-build rpmdevtools gcc-c++ cmake
```

Then build the package:

```bash
bash packaging/rpm/build-rpm.sh
```

The helper uses `/opt/monitoring-linux/rpmbuild` as its RPM build tree. On first use it prepares that directory for the current user; the actual RPM build runs without root privileges. The generated RPM will be under:

```text
/opt/monitoring-linux/rpmbuild/RPMS/x86_64/
```

## Standalone installation checklist

1. Copy the generated RPM to the target machine.
2. Install it with:

```bash
sudo dnf install -y ./linux-monitoring-1.0.0-15.el9.x86_64.rpm
```

3. Ensure SSH access is configured to the remote Linux hosts, ideally with key-based authentication.
4. Launch the app manually as your regular user:

```bash
monitoring-linux
```

5. Open the local dashboard in the browser:

```text
http://localhost:8080/
```

## Default configuration

The RPM installs the application files in `/opt/monitoring-linux` and the command link in `/usr/bin/monitoring-linux`. The default configuration is placed in:

```text
/etc/monitoring-linux/monitoring.conf
```

and contains the local port and SSH settings. SQLite is compiled into the application binary, so the target machine does not need the SQLite runtime package or command-line tool. SQLite data is stored in `~/.local/share/monitoring-linux/monitoring.db` for the user who starts the application, so it can run without root privileges.

The dashboard listens only on `127.0.0.1` and refreshes displayed data every 2.5 seconds without rebuilding unchanged content. Each monitored machine has its own panel containing its services and their status. Use Restart/Stop beside a service or Reboot/Shutdown in the host controls; stop and machine power actions require confirmation. The SSH account must be authorized to run the requested `systemctl` operations. Checks for up to four machines run concurrently, with one SSH session per machine for all configured services. The Terminal action opens an interactive SSH shell in the browser. It reuses a password held in memory or lets OpenSSH prompt inside the terminal; passwords are never written to SQLite and must be entered again after restarting the application. Adding or changing a target, or submitting its SSH password, wakes the monitor for an immediate next check.
