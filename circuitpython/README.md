Circuitpython version of the Current Designs 932 emulator.

Technically works (the best kind of works).

I removed the dependencies from this version (but left the folders), because I hadn't worked out exactly which dependencies I was using, and thus had the enire collection of Adafruit libraries in the `lib` subfolder.

Real box notes:
 - Has bInterval of 1
 - Has at least mouse and keyboard endpoints
 - In NAR mode, the scanner releases after ??? seconds

Caveats:
 - Uses the default bInterval of 8, which means that press/release events update every 8ms.
 - The python GC + display updates can probably lead to deadlines misses. Disabling the GC causes the device to fail in a few seconds.
 - Scanner sounds not implemented
 - Unsure whether the device would actually show up with the Current Designs vid/pid/name.
 - Most of the dependencies are unnecessary. I just haven't spent the time to figure out the dependency tree
 - Should blink LED(s) when triggers are running

A lot of these could probably be solved with an Arduino/C++ version. The UI/audio updates could be handled on the 2nd core, while the timing-critical things can happen on the first core.
