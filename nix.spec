%global nixbld_user "nix-builder-"
%global nixbld_group "nix-builders"

Summary: The Nix software deployment system
Name: nix
Version: 1.2
Release: 2%{?dist}
License: LGPLv2+
%if 0%{?rhel}
Group: Applications/System
%endif
URL: http://nixos.org/
Source0: %{name}-%{version}.tar.gz
%if 0%{?el5}
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
%endif
BuildRequires: perl(DBD::SQLite)
BuildRequires: perl(DBI)
BuildRequires: perl(WWW::Curl)
BuildRequires: perl(ExtUtils::ParseXS)
Requires: /usr/bin/perl
Requires: curl
Requires: perl-DBD-SQLite
Requires: bzip2
Requires: xz
BuildRequires: bzip2-devel
BuildRequires: sqlite-devel

# Hack to make that shitty RPM scanning hack shut up.
Provides: perl(Nix::SSH)
                                                               
%description
Nix is a purely functional package manager. It allows multiple
versions of a package to be installed side-by-side, ensures that
dependency specifications are complete, supports atomic upgrades and
rollbacks, allows non-root users to install software, and has many
other features. It is the basis of the NixOS Linux distribution, but
it can be used equally well under other Unix systems.

%package        devel
Summary:        Development files for %{name}
%if 0%{?rhel}
Group:          Development/Libraries
%endif
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description   devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.


%package doc
Summary:        Documentation files for %{name}
%if 0%{?rhel}
Group:          Documentation
%endif
BuildArch:      noarch
Requires:       %{name} = %{version}-%{release}

%description   doc
The %{name}-doc package contains documentation files for %{name}.


%package -n emacs-%{name}
Summary:        Nix mode for Emacs
%if 0%{?rhel}
Group:          Applications/Editors
%endif
BuildArch:      noarch
BuildRequires:  emacs
Requires:       emacs(bin) >= %{_emacs_version}

%description -n emacs-%{name}
This package provides a major mode for editing Nix expressions.

%package -n emacs-%{name}-el
Summary:        Elisp source files for emacs-%{name}
%if 0%{?rhel}
Group:          Applications/Editors
%endif
BuildArch:      noarch
Requires:       emacs-%{name} = %{version}-%{release}

%description -n emacs-%{name}-el
This package contains the elisp source file for the Nix major mode for
GNU Emacs. You do not need to install this package to run Nix. Install
the emacs-%{name} package to edit Nix expressions with GNU Emacs.


%prep
%setup -q
# Install Perl modules to vendor_perl
# configure.ac need to be changed to make this global; however, this will
# also affect NixOS. Use discretion.
%{__sed} -i 's|perl5/site_perl/$perlversion/$perlarchname|perl5/vendor_perl|' \
  configure


%build
extraFlags=
# - override docdir so large documentation files are owned by the
#   -doc subpackage
# - set localstatedir by hand to the preferred nix value
%configure --localstatedir=/nix/var \
           --docdir=%{_defaultdocdir}/%{name}-doc-%{version} \
           $extraFlags
make %{?_smp_flags}
%{_emacs_bytecompile} misc/emacs/nix-mode.el


%install
%if 0%{?el5}
rm -rf $RPM_BUILD_ROOT
%endif
make DESTDIR=$RPM_BUILD_ROOT install

find $RPM_BUILD_ROOT -name '*.la' -exec rm -f {} ';'

# Fix symlink: we want to link to the versioned soname, not to the
# unversioned one that'd be put in -devel
pushd $RPM_BUILD_ROOT%{perl_vendorarch}/auto/Nix/Store
ln -sf %{_libdir}/nix/libNixStore.so.0 Store.so
popd

# Specify build users group
echo "build-users-group = %{nixbld_group}" > $RPM_BUILD_ROOT%{_sysconfdir}/nix/nix.conf

# make per-user directories
for d in profiles gcroots;
do
  mkdir $RPM_BUILD_ROOT/nix/var/nix/$d/per-user
  chmod 1777 $RPM_BUILD_ROOT/nix/var/nix/$d/per-user
done

# fix permission of nix profile
# (until this is fixed in the relevant Makefile)
chmod -x $RPM_BUILD_ROOT%{_sysconfdir}/profile.d/nix.sh

# systemd not available on RHEL yet
%if ! 0%{?rhel} 
# install systemd service descriptor
mkdir -p $RPM_BUILD_ROOT%{_prefix}/lib/systemd/system
cp -p misc/systemd/nix-daemon.service \
  $RPM_BUILD_ROOT%{_prefix}/lib/systemd/system/
%endif

# Copy the byte-compiled mode file by hand
cp -p misc/emacs/nix-mode.elc $RPM_BUILD_ROOT%{_emacs_sitelispdir}/

# we ship this file in the base package
rm $RPM_BUILD_ROOT%{_defaultdocdir}/%{name}-doc-%{version}/README


%check
make check


%clean
rm -rf $RPM_BUILD_ROOT


%pre
getent group %{nixbld_group} >/dev/null || groupadd -r %{nixbld_group}
for i in $(seq 10);
do
  getent passwd %{nixbld_user}$i >/dev/null || \
    useradd -r -g %{nixbld_group} -G %{nixbld_group} -d /var/empty \
      -s %{_sbindir}/nologin \
      -c "Nix build user $i" %{nixbld_user}$i
done

%post
chgrp %{nixbld_group} /nix/store
chmod 1775 /nix/store
%if ! 0%{?rhel}
# Enable and start Nix worker
systemctl enable nix-daemon.service
systemctl start  nix-daemon.service
%endif

%files
%doc COPYING AUTHORS README
%{_bindir}/nix-*
%dir %{_libdir}/nix
%{_libdir}/nix/*.so.*
%{perl_vendorarch}/*
%exclude %dir %{perl_vendorarch}/auto/
%{_prefix}/libexec/*
%if ! 0%{?rhel}
%{_prefix}/lib/systemd/system/nix-daemon.service
%endif
%{_datadir}/emacs/site-lisp/nix-mode.el
%{_datadir}/nix
%{_mandir}/man1/*.1*
%{_mandir}/man5/*.5*
%{_mandir}/man8/*.8*
%config(noreplace) %{_sysconfdir}/profile.d/nix.sh
/nix
%dir %{_sysconfdir}/nix
%config(noreplace) %{_sysconfdir}/nix/nix.conf

%files devel
%{_includedir}/nix
%{_libdir}/nix/*.so

%files doc
%docdir %{_defaultdocdir}/%{name}-doc-%{version}
%{_defaultdocdir}/%{name}-doc-%{version}

%files -n emacs-%{name}
%{_emacs_sitelispdir}/*.elc
#{_emacs_sitestartdir}/*.el

%files -n emacs-%{name}-el
%{_emacs_sitelispdir}/*.el
