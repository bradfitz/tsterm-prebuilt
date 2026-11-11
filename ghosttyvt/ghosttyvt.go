// Package ghosttyvt provides a prebuilt WebAssembly build of libghostty-vt,
// the terminal emulator library from Ghostty (https://ghostty.org), linked
// with glue for tsterm (glue/tsterm.c). It is built by build.sh and used by
// github.com/bradfitz/tsterm/pkg/vt/ghosttyvt, which runs it with wazero.
//
// It lives in its own module so that tsterm's repository doesn't carry the
// history of a binary that changes with each rebuild.
package ghosttyvt

import _ "embed"

// Wasm is the WebAssembly module. Callers must check that its
// tsterm_abi_version export returns the ABIVersion they were written for.
//
//go:embed ghosttyvt.wasm
var Wasm []byte
