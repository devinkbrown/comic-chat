# IRC and IRCv3 specification map

## Core and framing

- Modern IRC client protocol: https://modern.ircdocs.horse/
- IRCv3 specification index: https://ircv3.net/irc/
- Capability negotiation and CAP 302: https://ircv3.net/specs/extensions/capability-negotiation
- Message tags and TAGMSG: https://ircv3.net/specs/extensions/message-tags
- Standard replies: https://ircv3.net/specs/extensions/standard-replies
- ISUPPORT: https://modern.ircdocs.horse/#rplisupport-005

## Registration and identity

- SASL 3.2: https://ircv3.net/specs/extensions/sasl-3.2
- Account tag: https://ircv3.net/specs/extensions/account-tag
- Account notify: https://ircv3.net/specs/extensions/account-notify
- Extended join: https://ircv3.net/specs/extensions/extended-join
- Away notify: https://ircv3.net/specs/extensions/away-notify
- Chghost: https://ircv3.net/specs/extensions/chghost
- Setname: https://ircv3.net/specs/extensions/setname
- Monitor: https://ircv3.net/specs/extensions/monitor
- Extended monitor: https://ircv3.net/specs/extensions/extended-monitor

## Tags, replies, and batches

- Batch: https://ircv3.net/specs/extensions/batch
- Labeled response: https://ircv3.net/specs/extensions/labeled-response
- Echo message: https://ircv3.net/specs/extensions/echo-message
- Server time: https://ircv3.net/specs/extensions/server-time
- Multiline: https://ircv3.net/specs/extensions/multiline
- Chat history: https://ircv3.net/specs/extensions/chathistory
- Read marker: https://ircv3.net/specs/extensions/read-marker
- Metadata: https://ircv3.net/specs/extensions/metadata
- Message redaction: https://ircv3.net/specs/extensions/message-redaction
- Account registration: https://ircv3.net/specs/extensions/account-registration
- STS: https://ircv3.net/specs/extensions/sts

Check the status banner on every IRCv3 page. Preserve draft/ prefixes for work-in-progress identifiers and do not silently promote draft behavior to a stable unprefixed name.

## Repository coverage

- Parser, state machine, and serializer: portable/include/comicchat/net/ircv3.hpp and portable/src/net/ircv3.cpp
- Standalone test program: portable/tests/ircv3_test.cpp
- Server fixtures: portable/tests/fixtures/irc/solanum.txt, unrealircd.txt, ircu.txt, orochi.txt, and inspircd.txt
- Legacy event adapter: v2.5-beta-1-modern/ircv3eventbridge.h
- Legacy connection adapter: v2.5-beta-1-modern/transportuibridge.h
- Historical consumers: v2.5-beta-1/ircproto.cpp, protsupp.cpp, ircsock.cpp, and histent.cpp

## Review questions

1. Does the spec define an actual CAP, a message tag, an ISUPPORT token, a batch type, or an ordinary command? Never request the wrong category.
2. Does the identifier remain opaque and case-sensitive?
3. Does the feature have normative dependencies, and are those dependencies about negotiation or only message interpretation?
4. Can replies span lines or batches?
5. What happens on CAP NEW, DEL, NAK, timeout, reconnect, malformed input, and resource exhaustion?
6. Does the legacy UI need a normalized event rather than raw wire data?
7. Which real server fixture proves the deployed shape?
