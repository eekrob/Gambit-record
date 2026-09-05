# Gambit Record Privacy Notice

Last updated: 2026-09-05

Gambit Record records gameplay only when the user starts recording. Finished recordings stay on the user's computer unless YouTube upload is enabled or the user explicitly chooses to upload.

When YouTube upload is enabled, Gambit Record sends the selected video and its generated title and description through the Gambit Record upload broker to YouTube. The broker streams the file and does not retain the video. Upload-session metadata is retained for up to 24 hours so interrupted uploads can resume. Basic abuse-prevention data, such as request counts associated with an IP address, may be processed to enforce service limits.

All users upload to the shared Gambit Record YouTube channel. Videos are requested with `unlisted` privacy. Anyone who receives an unlisted link may view and share it; YouTube may impose stricter visibility on uploads made by an unverified API project.

The desktop client does not contain the shared channel's Google OAuth credentials. OAuth credentials are stored as Cloudflare Worker secrets and are used only to obtain YouTube access tokens for upload and channel-identification requests.

Gambit Record does not sell personal data. Local configuration, recordings, and the persistent upload queue remain on the user's computer unless an upload is initiated. Users can disable YouTube uploads in `/grecord` and delete local recordings at any time.

For privacy questions, open an issue in the [Gambit Record repository](https://github.com/eekrob/Gambit-record/issues).
