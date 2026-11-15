package main

import "github.com/EchoTools/nevr-runtime/tools/internal/csymbol"

// csymbol64 is Echo VR's 64-bit name hash (internal/csymbol, the one Go copy).
func csymbol64(name string) uint64 { return csymbol.Hash(name) }
