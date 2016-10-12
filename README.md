# Commadorable 64

An adorable Commodore 64 desktop ornament. Blog post [here](http://johan.kanflo.com/the-commadorable-64/).

<p align="center">
  <img src="https://raw.githubusercontent.com/kanflo/commadorable-64/master/commodorable_64.png" alt="The Commadorable 64"/>
</p>

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

**Ordering the PCB**

The PCBs can be ordered from DirtyPCBs.com, [without touch](http://dirtypcbs.com/view.php?share=18116&accesskey=5cb9ea9c4754e5c9102b4350393b244e) or [with touch](http://dirtypcbs.com/view.php?share=18114&accesskey=f7144fbd88f9270b40ccc2829f08ee2f). If you use these links to order I will receive a $1 credit :)

