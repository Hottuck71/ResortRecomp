// Room codes: a host's IPv4 address and port in 11 letters and digits, like "K7Q2M-9XD4AR".
// Crockford base 32 (no I, L, O, or U), so a code read aloud is hard to get wrong, and the last character is a
// check, so a typo is caught before connecting. No server is involved: the code is the address.
#pragma once
#include <cstdint>
#include <string>

namespace room {
std::string encode(const std::string& ipv4, uint16_t port);  // "" if ipv4 isn't a dotted IPv4 address
bool decode(const std::string& code, std::string* ipv4, uint16_t* port);
std::string local_address();  // this PC's address on its home network, or 127.0.0.1
int self_test();
}  // namespace room
