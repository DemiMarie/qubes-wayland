%global _vpath_srcdir qubes-compositor-%{version}
Name: qubes-gui-agent-wayland
License: GPLv2-or-later
BuildRequires: pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(wayland-server)
BuildRequires: pkgconfig(wayland-scanner)
BuildRequires: (pkgconfig(wlroots) >= 0.16.0)
BuildRequires: (pkgconfig(wlroots) < 0.17.0)
BuildRequires: rustc
BuildRequires: cargo
BuildRequires: pkgconfig(xcb)
BuildRequires: qubes-libvchan-devel
BuildRequires: qubes-gui-common-devel
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(xkbcommon)
BuildRequires: pkgconfig(pixman-1)
BuildRequires: pkgconfig(pam)
BuildRequires: pkgconfig(libsystemd)
BuildRequires: qubes-db-devel
BuildRequires: meson
Version: 0.0.1
Release: 0
Summary: Experimental Wayland compositor for use in a Qubes OS VM.
Source0: qubes-compositor-%{version}.tar.xz

%prep
%autosetup -c

%build
find
%meson
%meson_build

%install
%meson_install

%description
Experimental Wayland compositor for use in a Qubes OS VM.  Only use
if you are willing to report bugs for stuff that does not work.

%files
%_bindir/qubes-compositor
%_bindir/qubes-gui-runuser-2
%_bindir/qubes-wayland-session
%_unitdir/qubes-gui-agent-wayland.service
%_presetdir/30_qubes-gui-agent-wayland.preset

%changelog
%autochangelog
