#pragma once

#include "core/Types.hpp"

#include <memory>
#include <string>

namespace orbit::core {

// A small HTTP proxy on 127.0.0.1 for the programs that can't be locked to a network
// interface themselves: yt-dlp (which can only pick a source address, and asks the system
// resolver for every name) and libtorrent's HTTP trackers (whose names libtorrent would
// look up with the system resolver too). Every name is looked up through the interface
// with InterfaceDns, and every connection is bound to the interface (SO_BINDTODEVICE), so
// while the lock is on nothing these programs do leaves over another connection - and it
// fails instead while the interface is down.
//
// It speaks CONNECT (for https) and plain absolute-form requests (http://host/path), and
// only to a client that sends its random credentials (Basic Proxy-Authorization). Linux
// only; elsewhere it never starts.
//
// With an upstream proxy (the user's own), every connection goes to that proxy instead,
// still through the interface: its name is looked up there, and a SOCKS5 proxy is given
// the target's name to resolve itself, so the user's proxy and the lock work together.
class InterfaceRelay {
public:
    InterfaceRelay();
    ~InterfaceRelay();
    InterfaceRelay(const InterfaceRelay&) = delete;
    InterfaceRelay& operator=(const InterfaceRelay&) = delete;

    // Relays through this interface from now on; empty stops the relay. The port and
    // credentials stay the same while it keeps running, so clients needn't be told again.
    // False when it should run but couldn't start listening.
    bool setInterface(const std::string& interfaceName);

    // The user's own proxy (HTTP or SOCKS5) every connection goes through; Type::None
    // connects directly. Takes effect for new connections.
    void setUpstream(const ProxySettings& upstream);

    // What to hand clients: an HTTP proxy at 127.0.0.1 with the credentials. Disabled
    // (Type::None) while the relay isn't running.
    [[nodiscard]] ProxySettings proxy() const;

    struct State;  // opaque; shared with the connection threads, which may outlive a stop

private:
    std::shared_ptr<State> state_;
    ProxySettings upstream_;  // kept here too, for a relay started later
};

}
