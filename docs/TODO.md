Things still to do.  (#n) indicates the issue tracker number.

 * ([#3](https://codeberg.org/a-j-wood/pv/issues/3)) Option ("`-x`"?) to use xterm title line for status (Joachim Haga)
 * ([#4](https://codeberg.org/a-j-wood/pv/issues/4)) Option for process title (Martin Sarsale) as "`pv - name:FooProcess -xyz - transferred: 1.3GB - 500KB/s - running: 10:15:30s`"
 * ([#6](https://codeberg.org/a-j-wood/pv/issues/6)) Look at effect of *O_SYNC* or `fsync` on performance; update counters during buffer flush
 * ([#9](https://codeberg.org/a-j-wood/pv/issues/9)) Option to switch rate to per minute if really slow
 * ([#10](https://codeberg.org/a-j-wood/pv/issues/10)) Add watchfd tests
 * ([#11](https://codeberg.org/a-j-wood/pv/issues/11)) Option "`--progress-from FILE`", read last number and use it as bytes read (Jacek Wielemborek)
 * ([#12](https://codeberg.org/a-j-wood/pv/issues/12)) Allow multiple "`-d`" options (Linus Heckemann for multiple PID:FD; Jacek Wielemborek)
 * ([#15](https://codeberg.org/a-j-wood/pv/issues/15)) Use Unicode for more granular progress bar (Alexander Petrossian)
 * ([#17](https://codeberg.org/a-j-wood/pv/issues/17)) Allow "`-r`" with "`-l`" and "`-n`" to output lines/sec (Roland Kletzing)
 * ([#22](https://codeberg.org/a-j-wood/pv/issues/22)) Options to skip input and seek on output (Jason A. Pfeil, Feb 2022)
 * ([#25](https://codeberg.org/a-j-wood/pv/issues/25)) Normalise progress to 100% on overrun (Andrej Gantvorg)
 * ([#34](https://codeberg.org/a-j-wood/pv/issues/34)) Continue timer even if input or output is blocking (Martin Probst - Jun 2017)
 * ([#35](https://codeberg.org/a-j-wood/pv/issues/35)) Allow decimal values for "`-s`", "`-L`", "`-B`" (Thomas Watson - Aug 2020)
 * ([#38](https://codeberg.org/a-j-wood/pv/issues/38)) Reset ETA on *SIGUSR1* (Jacek Wielemborek - Jan 2019)
 * ([#40](https://codeberg.org/a-j-wood/pv/issues/40)) Permit "`-c`" with "`-d PID:FD`", reject "`-N`" with "`-d PID`" (Norman Rasmussen - Nov 2020)
 * ([#43](https://codeberg.org/a-j-wood/pv/issues/43)) Differentiate between "`--eta`" and "`--fineta`" in display (André Stapf - Apr 2017)
 * ([#45](https://codeberg.org/a-j-wood/pv/issues/45)) Option "`--sparse`" (with block size option) to write sparse output (Andriy Galetski - Apr 2019)
 * ([#46](https://codeberg.org/a-j-wood/pv/issues/46)) Option to show speed gauge (% max speed) if progress not known (Ryan Cooley - Jun 2019)
 * ([#47](https://codeberg.org/a-j-wood/pv/issues/47)) Analyse splice and buffer usage to improve performance
 * ([#48](https://codeberg.org/a-j-wood/pv/issues/48)) Option to show multiple files with individual sizes and a cumulative total (Zach Riggle - Jul 2021)
 * ([#49](https://codeberg.org/a-j-wood/pv/issues/49)) Option to provide stats for avg/min/max/stddev throughput (Venky.N.Iyer)
 * ([#50](https://codeberg.org/a-j-wood/pv/issues/50)) Allow pv to report on a whole pipeline at once (Will Entriken - Feb 2011)
 * ([#54](https://codeberg.org/a-j-wood/pv/issues/54)) Run command every n percent ([haarp](https://github.com/haarp))
 * ([#56](https://codeberg.org/a-j-wood/pv/issues/56)) Support for backgrounding pv, and allowing it to be monitored separately ([jimbobmcgee](https://github.com/jimbobmcgee))
 * ([#67](https://codeberg.org/a-j-wood/pv/issues/67)) Wrap another process to monitor its stdin & stdout ([Alex Mason](https://github.com/axman6))
 * ([#76](https://codeberg.org/a-j-wood/pv/issues/76)) Provide a way to test translations without installing

Any assistance would be appreciated.
