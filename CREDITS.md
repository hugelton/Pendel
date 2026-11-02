# Credits

Pendel is Leo Kuroshita's ([@kurogedelic](https://github.com/kurogedelic), [Hügelton Instruments](https://hugelton.com)). It uses and
thanks the work below; each keeps its own licence (see [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and [LICENSES/](LICENSES/)).

- **[Hügelton Instruments](https://hugelton.com)** (Leo Kuroshita, [@kurogedelic](https://github.com/kurogedelic)):
  Pendel itself; from [Felucca](https://github.com/hugelton/Felucca), the screen renderer, the font tools, and the update:
  the package maker, the installers (`tools/pa_install.py`, `web/`) and, in pendel-bsp, the update loader; the
  [Fukiai](https://github.com/hugelton/Fukiai) icon font ([MIT](LICENSES/MIT-Fukiai.txt))
- Fonts: [Inter Tight](https://github.com/rsms/inter-tight) by The Inter Project Authors, [SIL OFL 1.1](LICENSES/OFL-InterTight.txt)
- Hardware layer: [pendel-bsp](https://github.com/hugelton/pendel-bsp) ([MPL-2.0](LICENSES/MPL-2.0-pendel-bsp.txt)), a branch of
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) (maintained by [charlesvestal](https://github.com/charlesvestal) and
  [keremimo](https://github.com/keremimo)) with the Pocket AMP board added; its authors in its `NOTICE`
- Boot-loader tools: [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) by Andrey Grigoryev ([kagaimiq](https://github.com/kagaimiq)) (MIT; credit only, not in this tree)
- References (algorithms, no code taken): Norman Koren's triode model, "Improved vacuum tube models for SPICE
  simulations" (the amp's 12AX7 curve, `tools/gen_tables.py`); Hal Chamberlin's state-variable filter
