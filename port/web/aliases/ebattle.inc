// draw_rect names draw_rect_store's storage (the PS2 build's object_fixups.json); other platforms alias
// it at link time, which wasm-ld cannot.
extern CRect_i_ draw_rect __attribute__((alias("draw_rect_store")));
