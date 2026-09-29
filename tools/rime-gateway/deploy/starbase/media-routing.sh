#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Policy routing: replies from UDP source ports 50000-50002 leave via eth1/blackStar.
set -euo pipefail

table=122

# Replies from the gateway's media sockets must leave through blackStar (10.77.0.1)
# so the edge's conntrack can reverse the DNAT it applied to arriving UDP frames.
# Without this return path, packets leave with the wrong source and media fails silently.

if ! ip route show table "$table" | grep -q "default via 10.77.0.1 dev eth1"; then
    ip route add default via 10.77.0.1 dev eth1 table "$table"
fi

if ! ip rule show | grep -q "lookup $table"; then
    ip rule add ipproto udp sport 50000-50002 lookup "$table"
fi

# coturn's egress restriction (see turn-egress.nft). Loaded here because it must be in place before
# coturn relays anything, and this unit runs before the services. Idempotent: the file deletes and
# recreates its own table.
nft -f /opt/rime/bin/turn-egress.nft
