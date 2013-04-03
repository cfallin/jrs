Name:       jrs
Version:	3.4
Release:	1
Summary:	Job-Running System

Group:		Application/System
License:	GPLv2+
URL:		http://c1f.net/jrs
Source:     http://c1f.net/jrs/packages/jrs-3.4.tar.bz2

BuildRequires:	apr-devel openssl-devel
Requires:	apr openssl

%description
JRS, or "Job-Running System", is a very basic batch system -- that is, a suite
of network software that can spawn "jobs" on a cluster of "compute nodes". In
this context, a "job" is just a command line -- for example, a simulator or a
script to perform a mathematical computation -- and a "compute node" is just an
ordinary Linux machine sitting on a network. JRS does not presume to know about
any other infrastructure -- for example, it does not know about any shared
filesystems or any network authentication systems. Its design is intentionally
simple: its only purpose is to dispatch command lines to remote hosts.

%prep
%setup -q

%build
scons %{?_smp_mflags}


%install
make install DESTDIR=%{buildroot}


%files
%doc README.md COPYING
%{_bindir}/jrs
%{_bindir}/jrs-daemon
%{_bindir}/jrs-submit
%{_sysconfdir}/init.d/jrs-daemon
%config(noreplace) %{_sysconfdir}/jrs/jrs.conf
%config(noreplace) %{_sysconfdir}/jrs/node.list
%config(noreplace) %{_sysconfdir}/jrs/node.master
%config(noreplace) %{_sysconfdir}/jrs/secret

%post
dd if=/dev/urandom of=%{_sysconfdir}/jrs/secret bs=256 count=1 >/dev/null 2>&1

%changelog
