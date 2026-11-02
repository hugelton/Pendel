# Contributing to Pendel

Thank you for helping. A few things keep the project in shape:

| What | Where |
| --- | --- |
| A bug | an issue, with the unit's firmware version, what you did and what happened |
| An idea or a request | Discussions |
| A change to the application, the simulator or the tools | a pull request here, under GPL-3.0-only |
| A change to the hardware layer (chip, board code) | a pull request to pendel-bsp, under MPL-2.0 |

- C11, freestanding (no libc beyond pendel-bsp's `chip/libc.c`), fixed point only: no floating point, no 64-bit
  multiply or divide in the firmware.
- Every file starts with its SPDX line and the copyright line.
- Comments and documents in English, concrete.
- `make` and `make ui` pass before a pull request; anything that touches the device is tried on a unit.
