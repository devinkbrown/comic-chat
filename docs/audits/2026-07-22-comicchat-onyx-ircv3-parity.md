# ComicChat / Onyx IRCv3 parity audit — 2026-07-22

## Scope and source lock

This audit compares ComicChat's IRCv3 capability catalog and client behavior
with the pinned Onyx Server submodule at `06bb3500b4fd62e2f307cb4004340c58062c0f59`.
The authoritative server registry is `cap_specs` in
`third_party/onyx-server/src/daemon/dispatch.zig:316-445`; the matching Onyx
reference is `third_party/onyx-server/docs/reference/ircv3-capabilities.md`.
It is a source comparison, not a claim about a public endpoint's runtime
policy: `sasl` and `sts` remain policy-gated by the server.

## Registry comparison

ComicChat's `published_client_capabilities` catalogs every capability advertised
by that pinned registry, including the `draft/no-implicit-names` alias. The
only catalog entries not advertised by this Onyx revision are
`draft/extended-isupport` and `draft/oper-tag`; they remain portable,
conditionally-requested support for compatible non-Onyx networks.

| Onyx registry area | Capability names | ComicChat result |
| --- | --- | --- |
| Core transport and identity | `server-time`, `message-tags`, `echo-message`, `sasl`, `multi-prefix`, `userhost-in-names`, `away-notify`, `setname`, `extended-join`, `invite-notify`, `account-tag`, `chghost`, `account-notify`, `extended-monitor` | Parsed or emitted through the normal message, identity, SASL, and NAMES paths. |
| History and conversations | `batch`, `draft/chathistory`, `draft/search`, `draft/event-playback`, `draft/read-marker`, `onyx/session-sync`, `onyx/bouncer`, `onyx/topics`, `draft/channel-rename` | BATCH/history/state processing exists. `SEARCH` and `MARKREAD` now have capability-gated outbound builders. The three `onyx/*` server behaviors remain safe to negotiate without secret material. |
| Message mutation and activity | `draft/message-redaction`, `draft/message-editing`, `draft/typing`, `draft/react`, `draft/reply`, `bot`, `draft/channel-context`, `draft/multiline` | Redaction/edit state, tags, multiline, and activity receive handling exist. `EDIT` and `REDACT` have capability-gated outbound builders; reply/reaction/typing accept either generic `message-tags` or their matching narrow cap. |
| Account, metadata, and responses | `draft/account-registration`, `draft/metadata-2`, `standard-replies`, `labeled-response`, `cap-notify`, `draft/pre-away` | Typed registration, standard response, label, and metadata state paths exist. `METADATA GET/LIST/SET/CLEAR` now has a capability-gated outbound builder. |
| Policy and topology | `no-implicit-names`, `draft/no-implicit-names`, `account-extban`, `utf8-only`, `draft/netsplit`, `draft/netjoin`, `sts` | Explicit NAMES and UTF-8 enforcement are consumed. Extban is server policy, and split/join BATCH frames use the existing BATCH parser. STS is deliberately observed but never requested. |

## Intentional non-negotiation

`onyx/e2ee` is cataloged but explicitly non-requestable. Negotiating it without
implementing the corresponding encryption and decryption control plane would
misrepresent client support and could expose unreadable message content. This
is the only Onyx CAP intentionally excluded from automatic selection.

## Wire-builder coverage added by this audit

The live client now rejects each command unless the corresponding negotiated
capability is enabled and validates the command's local IRC invariants before
queueing it:

| Capability | Wire form |
| --- | --- |
| `draft/search` | `SEARCH <target> :<query>` |
| `draft/message-editing` | `EDIT <target> <msgid> :<text>` |
| `draft/message-redaction` | `REDACT <channel> <msgid> [:reason]` |
| `draft/read-marker` | `MARKREAD <target> [timestamp=<rfc3339>\|*]` |
| `draft/metadata-2` | `METADATA <target> <GET\|LIST\|SET\|CLEAR> [key] [visibility] [:value]` |

The regression test negotiates the smallest dependency-complete CAP set and
asserts the exact queued command forms. Server authorization, edit ownership,
and metadata visibility continue to be enforced exclusively by Onyx.

## Residual integration work

This audit closes transport-builder gaps; presentation integration remains a
separate UI concern. In particular, searchable history controls, edit/redact
affordances, metadata editors, and topic controls should only be surfaced when
their negotiated capability is enabled. No UI path may bypass the client's
capability-gated transport methods.
