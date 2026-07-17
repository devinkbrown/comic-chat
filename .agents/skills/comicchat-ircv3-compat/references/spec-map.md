# IRC and IRCv3 specification map

## Core and framing

- Modern IRC client protocol: https://modern.ircdocs.horse/
- IRCv3 specification index: https://ircv3.net/irc/
- IRCv3 capability/tag/batch/ISUPPORT registry: https://ircv3.net/registry
- Capability negotiation and CAP 302: https://ircv3.net/specs/extensions/capability-negotiation
- Message tags and TAGMSG: https://ircv3.net/specs/extensions/message-tags
- Standard replies: https://ircv3.net/specs/extensions/standard-replies
- Extended ISUPPORT: https://ircv3.net/specs/extensions/extended-isupport
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
- Multi-prefix: https://ircv3.net/specs/extensions/multi-prefix
- Userhost in names: https://ircv3.net/specs/extensions/userhost-in-names
- Invite notify: https://ircv3.net/specs/extensions/invite-notify
- Channel rename: https://ircv3.net/specs/extensions/channel-rename
- Pre-away: https://ircv3.net/specs/extensions/pre-away
- No implicit names: https://ircv3.net/specs/extensions/no-implicit-names
- Network icon: https://ircv3.net/specs/extensions/network-icon

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
- Message IDs: https://ircv3.net/specs/extensions/message-ids
- Bot mode: https://ircv3.net/specs/extensions/bot-mode
- Oper tag: https://ircv3.net/specs/extensions/oper-tag
- Reply tag: https://ircv3.net/specs/client-tags/reply
- Typing tag: https://ircv3.net/specs/client-tags/typing
- Channel-context tag: https://ircv3.net/specs/client-tags/channel-context
- React tag: https://ircv3.net/specs/client-tags/react
- Account registration: https://ircv3.net/specs/extensions/account-registration
- STS: https://ircv3.net/specs/extensions/sts
- CTCP: https://modern.ircdocs.horse/ctcp.html
- DCC: https://modern.ircdocs.horse/dcc

Check the status banner on every IRCv3 page. The registry intentionally displays
draft entries without `draft/`; the individual specification controls the wire
identifier. Preserve draft prefixes for work-in-progress identifiers and do not
silently promote draft behavior to a stable unprefixed name.

## Repository coverage

- Parser, state machine, and serializer: portable/include/comicchat/net/ircv3.hpp and portable/src/net/ircv3.cpp
- Standalone test program: portable/tests/ircv3_test.cpp
- Server fixtures: portable/tests/fixtures/irc/solanum.txt, unrealircd.txt, ircu.txt, orochi.txt, and inspircd.txt
- Legacy event adapter: v2.5-beta-1-modern/ircv3eventbridge.h
- Legacy connection adapter: v2.5-beta-1-modern/transportuibridge.h
- Historical consumers: v2.5-beta-1/ircproto.cpp, protsupp.cpp, ircsock.cpp, and histent.cpp
- Product-level status and automatic-request safety: docs/IRCv3-COVERAGE.md

The coverage ledger is a maintained artifact, not an audit snapshot to ignore.
Update it whenever a capability, tag, batch type, ISUPPORT token, fixture, typed
event, legacy adapter, or support claim changes. “Implemented” requires product
consumption where the extension has visible/model effects; parser-only support is
partial or observe-only.

## Review questions

1. Does the spec define an actual CAP, a message tag, an ISUPPORT token, a batch type, or an ordinary command? Never request the wrong category.
2. Does the identifier remain opaque and case-sensitive?
3. Does the feature have normative dependencies, and are those dependencies about negotiation or only message interpretation?
4. Can replies span lines or batches?
5. What happens on CAP NEW, DEL, NAK, timeout, reconnect, malformed input, and resource exhaustion?
6. Does the legacy UI need a normalized event rather than raw wire data?
7. Which real server fixture proves the deployed shape?
