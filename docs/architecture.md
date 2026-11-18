# KaiDump architecture

KaiDump is intentionally split into a small application layer and reusable
output backends. The application layer owns the current selection and decides
what should be exported. The backends only describe, package, or transfer the
selected files.

## Application flow

1. `main.c` initializes input, logging, and the renderer.
2. `app.c` checks the kaiHEN protocol and confirms hbloader supplied the
   `fsp-ldr` override required by libounce.
3. `catalog.c` asks NS for accessible applications, loads their metadata, and
   builds the selectable base/update/DLC rows.
4. `screens.c` renders the current `AppState` and turns controller presses into
   state changes.
5. `export.c` translates the selected rows into one of the three output forms
   and sends it to SD, FTP, or USB.

`app_internal.h` is private to the application layer. It contains the shared
state and the small set of functions that cross those module boundaries. It is
not intended as a public library API.

## Content access

`content_access.c` is the console-facing part of the dumper. It resolves NCM
content, prepares rights when required, and opens ExeFS and RomFS. It also owns
the temporary program-location/index-map handling needed by different title
layouts. Keeping this in one module makes it easier to audit changes that can
affect Switch 1 and Switch 2 compatibility.

`content_export.c` is deliberately simpler. Once content has been mounted, it
walks the files and writes extracted output. It has no title-discovery or UI
responsibilities.

## Output backends

- Extracted output uses the virtual tree for inventory and direct file access.
- TAR presents that same tree as a sequential, uncompressed archive.
- NUP inventories and hashes the tree before exposing or writing the archive.
- FTP serves either individual virtual files or one virtual archive.
- USB exposes the same objects through the embedded MTP responder.

FTP and USB do not stage game data on the SD card. SD output is the only path
that creates dump files locally.

## Embedded MTP code

`source/mtp/` is a source-level integration of the OunceMTP components KaiDump
uses. Keep changes to those files separate from KaiDump-specific changes when
possible. If the code is refreshed from upstream, update
`source/mtp/README.md`, retain the SPDX notices, and confirm the ISC license in
`licenses/OunceMTP-ISC.txt` is still current.
