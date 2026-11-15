#pragma once

#include <string>
#include <vector>

namespace orbit::core {

// Name lookups for downloads locked to one network interface (a VPN's wg0, tun0). The
// system resolver can't be told which interface to use, so with the lock on a lookup
// would still go out over the normal connection and reveal which sites are downloaded
// from. These lookups go as DNS over HTTPS to a resolver addressed by IP (Quad9, then
// Cloudflare), through the locked interface itself, so they follow the same route as
// the downloads, or fail with them while the interface is down.
class InterfaceDns {
public:
    // IPv4 and IPv6 addresses of host, looked up through the interface. Cached for the
    // records' lifetime (at least 1 and at most 10 minutes). Empty when the lookup failed
    // (the interface is down, or the name doesn't exist). An IP literal comes back as is.
    static std::vector<std::string> resolve(const std::string& host, const std::string& interfaceName);

    // Lower-case host of an http(s) URL without brackets, user info or port; empty if none.
    static std::string hostOf(const std::string& url);
    // The URL's port: the explicit one, or 443/80 by scheme.
    static int portOf(const std::string& url);
    static bool isIpLiteral(const std::string& host);

    // Builds the DNS query (RFC 1035 wire format) for one name and record type, and reads
    // the addresses (A = 1, AAAA = 28) out of an answer. Exposed for the tests.
    static std::string buildQuery(const std::string& host, int type);
    static std::vector<std::string> parseAnswer(const std::string& message, int& ttlSeconds);

    static void clearCache();
};

}
