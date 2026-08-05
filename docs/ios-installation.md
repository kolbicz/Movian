# Installing Movian on iOS and iPadOS

The current Movian 7.0.272 IPA, conventional rootless package, and RootHide
package require iOS or iPadOS 15 or newer on an arm64 device. Choose one
installation method; do not install the IPA and a jailbreak package at the
same time because both provide the same application.

## TrollStore

TrollStore permanently installs IPA files, but it only supports specific OS
versions. Its official compatibility list currently covers iOS 14.0 beta 2
through 16.6.1, iOS 16.7 RC (20H18), and iOS 17.0. Other 16.7 releases and
iOS 17.0.1 or newer are not supported.

1. Confirm that the exact device and OS version has a supported TrollStore
   installation method using the guide linked by the TrollStore project.
2. Download `Movian-iOS-7.0.272-unsigned.ipa` on the device.
3. Open the IPA with TrollStore, then choose **Install**.
4. Future Movian releases can be installed over the existing app. Uninstall
   TrollStore-installed apps from inside TrollStore.

Official resources:

- [TrollStore project and compatibility](https://github.com/opa334/TrollStore)
- [TrollStore installation guides](https://ios.cfw.guide/installing-trollstore/)

## Sideloading with an Apple ID

The unsigned IPA can also be signed and installed by a desktop sideloading
tool such as Sideloadly or AltStore. A free Apple ID normally gives the app a
seven-day signing period and a limited number of active apps; a paid developer
account permits longer-lived development signatures. iOS 16 and newer may
require **Settings > Privacy & Security > Developer Mode**.

### Sideloadly

1. Install [Sideloadly](https://sideloadly.io/) on macOS or Windows.
2. Connect the device, select the Movian IPA, enter the Apple ID used for
   signing, and start the installation.
3. Trust/enable the development app if iOS requests it, then open Movian.
4. Re-sign before the profile expires. Sideloadly can refresh apps when its
   refresh service and the device are available.

### AltStore

1. Follow the [official AltStore installation guide](https://faq.altstore.io/).
2. Transfer the Movian IPA to the device and open it from AltStore's **My
   Apps** section.
3. Keep AltServer reachable and refresh the app before its signing period
   expires.

### Impactor

[Impactor](https://github.com/claration/Impactor) is an open-source,
cross-platform sideloading application for macOS, Windows, and Linux. It can
sign and install the Movian IPA with an Apple Account.

1. Download the current Impactor build from its
   [releases page](https://github.com/claration/Impactor/releases).
2. Connect and trust the iOS or iPadOS device, then select
   `Movian-iOS-7.0.272-unsigned.ipa` in Impactor.
3. Sign and install it using the selected Apple Account. With a free account,
   reinstall or refresh the app before its seven-day signing period expires.

Do not download re-signed Movian copies from unknown IPA sites. Use the IPA
attached to this repository's GitHub release and let the selected installer
sign it locally.

## Jailbroken devices

The release supplies two Debian packages for package managers such as Sileo
and Zebra:

- `Movian-iOS-7.0.272-rootless.deb`: modern rootless jailbreaks, Debian
  architecture `iphoneos-arm64`, iOS 15 or newer, installed below `/var/jb`.
  It includes the platform-app and GPU/IOSurface permissions needed by Movian.
- `Movian-iOS-7.0.272-roothide.deb`: RootHide environments on iOS 15 or newer;
  Debian architecture `iphoneos-arm64e`.

Download the package matching the jailbreak, open it in the package manager,
and install it. The package refreshes the application cache automatically. If
the icon does not appear, refresh the icon cache or respring once. Remove it
through the same package manager.

The `.deb` files contain an ad-hoc signed application and require an active
jailbreak. They cannot install Movian on a stock device. RootHide is distinct
from conventional rootless: its package environment maps `/Applications` into
a randomized jailbreak root. The RootHide package includes the application
entitlements required by the official RootHide developer specification.

Confirm that the exact RootHide environment, device and iOS version are
supported before installing packages, and keep a working backup. This package
does not install or modify the jailbreak itself. It includes the GPU and
IOSurface user-client entitlements required by Movian's OpenGL ES renderer.

To rebuild both packages after building the IPA:

```sh
./Autobuild/ios-deb.sh
```

The packaging script requires `ldid` (`brew install ldid` on macOS). RootHide
packaging follows the [official RootHide developer guide](https://github.com/roothide/Developer).
The three-part application/core version remains aligned with the corresponding
M7 source release. Apple-only revisions use a fourth public component (for
example `7.0.272.001`) for release tags, artifacts and Debian packages, while
the core/plugin version and Apple `CFBundleShortVersionString` remain
`7.0.272`. A new M7 source import advances the first three components instead.
