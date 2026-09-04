# Security policy

Do not report OAuth credentials in a public issue. Revoke an exposed YouTube refresh token immediately in the Google account, rotate `BROKER_KEY`, redeploy the Cloudflare Worker, and rebuild the release binaries.

The shared broker key is an abuse-control identifier, not a secret capable of minting Google tokens. It is expected to be recoverable from `GambitRecord.exe`; IP/global limits and channel-side monitoring are therefore mandatory.
