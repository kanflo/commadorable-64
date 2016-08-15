# Commadorable 64

An adorable Commodore 64 desktop ornament. Blog post [here](http://johan.kanflo.com/the-commadorable-64/).

**Howto**


Assuming you have cloned the EOR git and set your wifi credentials:

```
export EOR_ROOT=/path/to/esp-open-rtos
cd $EOR_ROOT
git clone https://github.com/kanflo/eor-arduino-compat.git extras/arduino-compat
git clone https://github.com/kanflo/eor-adafruit-ili9341.git extras/ili9341
git clone https://github.com/kanflo/eor-cli.git extras/cli
cd /path/to/wherever
git clone https://github.com/kanflo/commadorable-64.git
cd commadorable-64
make -j8 && make flash
```
