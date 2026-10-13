# MonitoringLinux

MonitoringLinux is a standalone Linux monitoring application designed to run on one control node and monitor other Linux machines in the same LAN using SSH. The project ships a local web dashboard and stores monitoring data in SQLite.

## Main goal

Install the RPM on a single Red Hat 9 / AlmaLinux machine, launch the binary from the command line, open the local web UI, and add the IPs and services of the remote Linux hosts you want to monitor.

## Features

- local web dashboard served on localhost
- SSH-based checks against remote Linux VMs
- monitoring of service names such as sshd, nginx, httpd, postgresql, etc.
- persistent configuration in SQLite
- no external web service required
- portable RPM package intended for LAN-only monitoring

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/monitoring-linux --config /etc/monitoring-linux/monitoring.conf
```

## Web UI

Open the browser on the same machine:

```text
http://localhost:8080/
```

## SSH access requirement

The control machine must be able to SSH into the target Linux hosts. The most practical setup is an SSH key-based authentication.

Example:

```bash
ssh-copy-id root@10.0.0.22
```

## RPM packaging on Red Hat 9 / AlmaLinux

The RPM spec is available in `packagin/rpm/linux-monitoring.spec`.

This project is intentionally designed as a manual-start application:
- no automatic systemd activation
- no external package installation required beyond the standard OS base libraries
- installed from a standalone RPM and launched manually from the command line

Install the build prerequisites on the build machine:

```bash
dnf install -y rpm-build rpmdevtools gcc-c++ cmake sqlite-devel
```

Then build the package:

```bash
./packagin/rpm/build-rpm.sh
```

or directly:

```bash
rpmbuild -bb packagin/rpm/linux-monitoring.spec
```

The generated RPM will be in the standard RPM output tree, usually:

```text
~/rpmbuild/RPMS/x86_64/
```

## Standalone installation checklist

1. Copy the generated RPM to the target machine.
2. Install it with:

```bash
rpm -Uvh ./linux-monitoring-1.0.0-1.el9.x86_64.rpm
```

3. Ensure SSH access is configured to the remote Linux hosts, ideally with key-based authentication.
4. Launch the app manually from the command line:

```bash
/usr/sbin/monitoring-linux --config /etc/monitoring-linux/monitoring.conf
```

5. Open the local dashboard in the browser:

```text
http://localhost:8080/
```

## Default configuration

The default configuration is placed in:

```text
/etc/monitoring-linux/monitoring.conf
```

and contains the local port, local SQLite path and the SSH-related settings used by the monitor.
