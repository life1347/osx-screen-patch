osx-screen-patch
================

Patching GNU Screen with vertical split support in OS X
AUTO
=====
```bash
$ cd screen/rc
$ ./patch.sh
```
MANUAL 
====
``` bash
$ cd screen/rc
$ ./configure --enable-locale --enable-telnet --enable-colors256 --enable-rxct_osc
$ make
$ sudo make install
```
