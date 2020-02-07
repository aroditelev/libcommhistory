Name:       libcommhistory-qt5
Summary:    Communications event history database API
Version:    1.9.44
Release:    1
Group:      System/Libraries
License:    LGPLv2
URL:        https://git.sailfishos.org/mer-core/libcommhistory
Source0:    %{name}-%{version}.tar.bz2
Source1:    %{name}.privileges
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Test)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions) >= 0.1.41
BuildRequires:  pkgconfig(contactcache-qt5) >= 0.0.17
BuildRequires:  libphonenumber-devel

%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}

%description
Library for accessing the communications (IM, SMS and call) history database.

%package unit-tests
Summary: Unit Test files for libcommhistory
Requires: blts-tools

%description unit-tests
Unit Test files for libcommhistory

%package performance-tests
Summary: Performance Test files for libcommhistory

%description performance-tests
Performance Test files for libcommhistory

%package tools
Summary: Command line tools for libcommhistory
Requires: %{name} = %{version}-%{release}
Conflicts: libcommhistory-tools

%description tools
Command line tools for the commhistory library.

%package declarative
Summary: QML plugin for libcommhistory
License: BSD and LGPLv2
Requires: %{name} = %{version}-%{release}

%description declarative
QML plugin for libcommhistory

%package devel
Summary: Development files for libcommhistory
Requires: %{name} = %{version}-%{release}

%description devel
Headers and static libraries for the commhistory library.

%package doc
Summary: Documentation for libcommhistory

%description doc
Documentation for libcommhistory

%prep
%setup -q -n %{name}-%{version}

%build
unset LD_AS_NEEDED
%qtc_qmake5 "PROJECT_VERSION=$(sed 's/+.*//' <<<"%{version}")"
%qtc_make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake_install

mkdir -p %{buildroot}%{_datadir}/mapplauncherd/privileges.d
install -m 644 -p %{SOURCE1} %{buildroot}%{_datadir}/mapplauncherd/privileges.d

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%post unit-tests
for n in ut_recentcontactsmodel ut_callmodel; do
    pathname=/opt/tests/libcommhistory-qt5-unit-tests/$n
    chgrp privileged $pathname && chmod g+s $pathname
done

%files
%defattr(-,root,root,-)
%{_libdir}/libcommhistory-qt5.so*
%license COPYING

%files tools
%defattr(-,root,root,-)
%{_bindir}/commhistory-tool
%{_datadir}/mapplauncherd/privileges.d/*

%files declarative
%defattr(-,root,root,-)
%{_libdir}/qt5/qml/org/nemomobile/commhistory

%files unit-tests
%defattr(-,root,root,-)
/opt/tests/libcommhistory-qt5-unit-tests

%files performance-tests
%defattr(-,root,root,-)
/opt/tests/libcommhistory-qt5-performance-tests

%files devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/commhistory-qt5.pc
%{_includedir}/commhistory-qt5

%files doc
%defattr(-,root,root,-)
%{_datadir}/doc/libcommhistory-qt5
