// Firmware version — shown in the web UI tagline and stamped into the build
// artefact's filename.
//
// This default is the single source of truth: the Makefile reads BRIDGE_VERSION
// from here so an unversioned `make <board>` still stamps and names the image
// with it. The release workflow overrides it at build time with
// -DBRIDGE_VERSION=<tag> (see CMakeLists.txt), hence the #ifndef guard.
#pragma once

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "2026.6.0-alpha"
#endif
