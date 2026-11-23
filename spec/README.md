# spec

TLA+ specifications of the parts of clockwork where the order of steps
between threads is the whole question. Each restates the code it names, a
shared-memory read or write to a step, and TLC tries every order of them.

- `Ring.tla`: a message ring with several writers and one reader, the word
  the reader sleeps on, and the handshake that pauses the reader.
  - `Ring.cfg`: the ring and the sleep. About a minute.
  - `RingPause.cfg`: the pause and the stop. About five minutes.

```sh
java -cp tla2tools.jar tlc2.TLC -config spec/Ring.cfg spec/Ring.tla
java -cp tla2tools.jar tlc2.TLC -config spec/RingPause.cfg spec/Ring.tla
```

`tla2tools.jar` is from https://github.com/tlaplus/tlaplus/releases. TLC
leaves a `states` folder beside where it is run, of a gigabyte or more:
pass `-metadir` to put it elsewhere, and delete it after.
