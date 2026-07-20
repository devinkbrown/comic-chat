//! Narrow module boundary for the pinned Onyx TLS implementation.
//!
//! ComicChat deliberately imports only the TLS client, signing key support,
//! and client-certificate loader. This keeps unrelated Onyx Server packages
//! out of the client build and its source distribution.

pub const crypto = struct {
    pub const tls_client = @import("third_party/onyx-server/src/crypto/tls_client.zig");
    pub const sign = @import("third_party/onyx-server/src/crypto/sign.zig");
};

pub const daemon = struct {
    pub const tls_certs = @import("third_party/onyx-server/src/daemon/tls_certs.zig");
};
