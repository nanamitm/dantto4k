#pragma once

// TSDuck reads the digital-TV name tables it uses for CA system families and
// for every displayed identifier from tsduck.dtv.names. It looks for that file
// on $TSPLUGINS_PATH, next to the executable, on $PATH and under %TSDUCK%, and
// complains on stderr - "configuration file 'dtv' not found" - once per lookup
// when none of them has it.
//
// A dantto4k release ships a single binary and no installed TSDuck, so on a
// machine without one every run started with that error. The file is built into
// the binary as a resource instead: this unpacks it under the user's temporary
// directory and points TSDuck at it.
//
// Call it before anything reaches the TSDuck library - the first statement of
// main() or of an exported entry point. It does nothing after the first call,
// and nothing at all off Windows, where TSDuck is an installed package that
// brings its own copy.
void ensureTsduckNamesAvailable();
