// See room_code.h.
#include "room_code.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <cstdio>

namespace room {
namespace {
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int value_of(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c == 'O') c = '0';            // read as the digit
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; i++)
        if (kAlphabet[i] == c) return i;
    return -1;
}

bool parse_ipv4(const std::string& s, uint32_t* out) {
    unsigned a, b, c, d;
    char extra;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = a << 24 | b << 16 | c << 8 | d;
    return true;
}

int check_of(uint64_t v) {
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += int((v >> (i * 5)) & 31) * (2 * i + 1); /* odd weights: every single typo changes it */
    return sum % 32;
}
}  // namespace

std::string encode(const std::string& ipv4, uint16_t port) {
    uint32_t ip;
    if (!parse_ipv4(ipv4, &ip)) return "";
    uint64_t v = uint64_t(ip) << 16 | port;  // 48 bits, so 10 characters
    std::string code;
    for (int i = 9; i >= 0; i--) {
        code.push_back(kAlphabet[(v >> (i * 5)) & 31]);
        if (i == 5) code.push_back('-');
    }
    code.push_back(kAlphabet[check_of(v)]);
    return code;
}

bool decode(const std::string& code, std::string* ipv4, uint16_t* port) {
    uint64_t v = 0;
    int count = 0, check = -1;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        int x = value_of(c);
        if (x < 0) return false;
        if (count < 10) v = v << 5 | uint64_t(x);
        else if (count == 10) check = x;
        else return false;
        count++;
    }
    if (count != 11 || check != check_of(v) || (v >> 48) != 0) return false;
    uint32_t ip = uint32_t(v >> 16);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    *ipv4 = buf;
    *port = uint16_t(v & 0xFFFF);
    return true;
}

std::string local_address() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // The address a connection out would use, found without sending anything: connecting a UDP socket only
    // picks the route.
    std::string result = "127.0.0.1";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        inet_pton(AF_INET, "192.168.0.1", &to.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
            sockaddr_in me{};
            int len = sizeof me;
            if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
                char buf[32];
                inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf);
                result = buf;
            }
        }
        closesocket(s);
    }
    return result;
}

int self_test() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) failures++;
    };
    std::string ip;
    uint16_t port = 0;
    std::string code = encode("192.168.1.42", 51010);
    check(code.size() == 12 && code[5] == '-', "a code is 11 characters with a dash");
    check(decode(code, &ip, &port) && ip == "192.168.1.42" && port == 51010, "a code decodes to its address");
    std::string lower = code;
    for (char& c : lower) c = char(std::tolower(c));
    check(decode(lower, &ip, &port) && ip == "192.168.1.42", "lower case works");
    std::string typo = code;
    typo[2] = typo[2] == 'A' ? 'B' : 'A';
    check(!decode(typo, &ip, &port), "a typo is caught");
    check(encode("300.1.1.1", 1).empty() && encode("example.com", 1).empty(), "only IPv4 addresses are encoded");
    check(decode(encode("255.255.255.255", 65535), &ip, &port) && ip == "255.255.255.255" && port == 65535,
          "the largest address round-trips");
    std::printf("Room code self-test: %d failures\n", failures);
    return failures;
}
}  // namespace room
