#!/bin/sh
./configure --enable-locale --enable-telnet --enable-colors256 --enable-rxct_osc
make
sudo make install clean
