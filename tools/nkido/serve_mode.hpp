#pragma once

#include "bytecode_loader.hpp"

namespace nkido {

// Headless persistent mode for editor integration. Reads newline-delimited
// JSON commands from stdin and emits newline-delimited JSON events to stdout.
//
// Commands (one per line):
//   {"cmd":"load","source":"...","uri":"file:///..."}  Compile + hot-swap (or cold-start)
//   {"cmd":"stop"}                                      Pause playback (state preserved)
//   {"cmd":"set_param","name":"...","value":<num>}      Update a param() value
//   {"cmd":"quit"}                                      Exit the process
//
// Events (one per line):
//   {"event":"ready"}
//   {"event":"compiled","ok":true|false}
//   {"event":"diagnostic","severity":"...","code":"...","message":"...","range":{...}}
//   {"event":"stopped"}
//   {"event":"param_changed","name":"...","value":<num>,"ok":true|false}
//   {"event":"error","message":"..."}
int run_serve_mode(const Options& opts);

}  // namespace nkido
