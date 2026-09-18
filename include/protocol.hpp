#pragma once
// A small, human-readable text protocol (newline-delimited, similar in
// spirit to Redis's original inline commands - not RESP, kept simple on
// purpose so you can `nc` or `telnet` into the server and try it by hand).
//
// Supported commands:
//   SET <key> <value>            -> OK
//   SET <key> <value> EX <secs>  -> OK  (with TTL)
//   GET <key>                    -> VALUE <value>  |  NOT_FOUND
//   DEL <key>                    -> DELETED  |  NOT_FOUND
//   EXISTS <key>                 -> TRUE  |  FALSE
//   EXPIRE <key> <secs>          -> OK  |  NOT_FOUND
//   PING                         -> PONG
// Anything else                  -> ERROR <message>

#include <string>

#include "kv_store.hpp"

namespace protocol {

// Parses and executes a single command line against `store`.
// Returns the response line to write back to the client (no trailing \n).
std::string handle_command(KVStore& store, const std::string& line);

}  // namespace protocol
