// The native test env doesn't build src/ (test_build_src = no, since main.cpp
// and the BLE/UI layers need Arduino). Pull in just the portable protocol
// implementation files here.
#include "../../src/ef/crc.cpp"
#include "../../src/ef/crypto.cpp"
#include "../../src/ef/keydata.cpp"
#include "../../src/ef/packet.cpp"
#include "../../src/ef/river2.cpp"
