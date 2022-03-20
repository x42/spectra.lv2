spectra.lv2 - Spectr Spectrum Analyzer
======================================

spectra.lv2 is lollipop graph spectrum analyzer.

It is available as [LV2 plugin](http://lv2plug.in/) and standalone [JACK](http://jackaudio.org/)-application.

Install
-------

Compiling spectr.lv2 requires the LV2 SDK, jack-headers, gnu-make, a c++-compiler,
libpango, libcairo and openGL (sometimes called: glu, glx, mesa).

```bash
  git clone https://github.com/x42/spectra.lv2.git
  cd spectra.lv2
  make submodules
  make
  sudo make install PREFIX=/usr
```

Note to packagers: the Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CXXFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CXXFLAGS`), also
see the first 10 lines of the Makefile.
You really want to package the superset of [x42-plugins](https://github.com/x42/x42-plugins).


Screenshots
-----------

![screenshot](https://raw.github.com/x42/spectra.lv2/master/img/spectr.png "Spectr LV2 GUI - 100Hz Sine")
