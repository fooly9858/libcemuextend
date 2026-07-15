# libcemuextend

`libcemuextend` is the byte-exact CemuExtend Bridge wire ABI and the Wii U guest SDK.
It deliberately contains no Cemu or Minecraft types. The wire format is big-endian,
versioned as ABI 1.0, and shared by Cemu's HLE host and PowerPC guests.

The default 256 KiB shared-memory region contains a 256-byte bridge header, a 2 KiB
service directory, independent host and guest state pages, four SPSC rings, and two
64 KiB bulk blocks. `ValidateLayout` rejects unaligned, overlapping, overflowing, or
otherwise inconsistent registrations before either endpoint starts accessing them.

## Guest setup

Configure the platform callbacks after the title's DynLoad and allocator functions
are available, then initialize and pump the client from the game's update thread.

```cpp
cemuextend::guest::ConfigurePlatform(callbacks);
cemuextend::guest::Client bridge;
bridge.Initialize();

// Once per frame, on the same thread that owns callbacks:
bridge.Pump();
```

Requests from other threads enter a bounded queue. Response and event callbacks run
only from `Pump()`. Bulk blocks are generation-checked and released automatically
after a receiving endpoint has copied their contents.

## Native verification

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
