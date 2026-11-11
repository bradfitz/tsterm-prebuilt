package ghosttyvt

import (
	"bytes"
	"testing"
)

func TestWasm(t *testing.T) {
	if !bytes.HasPrefix(Wasm, []byte("\x00asm\x01\x00\x00\x00")) {
		t.Fatalf("Wasm is not a version 1 WebAssembly module (%d bytes)", len(Wasm))
	}
	if ABIVersion < 1 || LibghosttyTag == "" || ZigVersion == "" {
		t.Errorf("version.go incomplete: ABI %d, tag %q, zig %q", ABIVersion, LibghosttyTag, ZigVersion)
	}
}
