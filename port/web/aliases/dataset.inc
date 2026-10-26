// WorkBuffer__2 names WorkBuffer's storage (the PS2 build's object_fixups.json); other platforms alias
// it at link time, which wasm-ld cannot.
extern "C" CDataAlloc2<1> *WorkBuffer__2 __attribute__((alias("WorkBuffer")));
