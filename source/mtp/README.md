# OunceMTP integration

This directory contains the part of
[OunceMTP](https://github.com/venustrikes/ouncemtp) used by KaiDump's USB
output. It is intentionally not a copy of the complete application.

Imported from OunceMTP v1.0.0, revision
`46c3056251e21e3a39a92ddc9b9881b4df6ccc2a`:

- `dataset.*` — MTP dataset encoding and text conversion
- `mtp.*` — portable MTP responder
- `objdb.*` — object handle database
- `status.*` — worker/UI status sharing
- `transport.h` and `usb_transport.c` — Switch USB transport

The files retain their original SPDX notices and are distributed under the ISC
license in `licenses/OunceMTP-ISC.txt`.

KaiDump supplies its own virtual filesystem callbacks and worker lifecycle in
`source/export.c`; OunceMTP's standalone UI, entry point, assets, tests, and
build files are not needed.
