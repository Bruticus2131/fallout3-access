# Fallout 3 Access — Nexus Mods API and SSO usage

This page describes exactly how the Fallout 3 Access installer talks to Nexus Mods.
It accompanies our request for an SSO application id.

## What the installer does

The installer prepares Fallout 3 for blind and visually impaired players. It installs:

- FOSE (from the FOSE website),
- the Fallout 3 Access plugin (from our GitHub releases),
- Loot Menu Updated ([mod 27191](https://www.nexusmods.com/fallout3/mods/27191)), and
- for Steam/GOG copies only, the Anniversary Patcher ([mod 24913](https://www.nexusmods.com/fallout3/mods/24913)),
  which downgrades the game to 1.7.0.3 so that FOSE can run.

Only the two Nexus mods involve the Nexus Mods API.

## Sign-in

The user chooses one of three ways to get the Nexus files:

1. **Download them manually** (default; no account type required). The installer opens the mod page and the
   user points it at the downloaded archive. No API call is made.
2. **Paste a personal API key** (Premium accounts).
3. **Sign in with Nexus Mods (SSO)** (Premium accounts) — the subject of this request.

The SSO flow follows [sso-integration-demo](https://github.com/Nexus-Mods/sso-integration-demo) exactly:
a websocket to `wss://sso.nexusmods.com`, a random request id, the authorise page
`https://www.nexusmods.com/sso?id=<id>&application=<slug>` opened in the user's browser, and the API key
received on the socket. On a dropped connection the installer reconnects with the same id and the
connection token. It gives up after five minutes. Code: `Get-NexusKeyViaSso` in
[`installer/f3a-install.ps1`](../../installer/f3a-install.ps1).

## Personal keys and registration

Nexus Mods' [API Acceptable Use Policy](https://help.nexusmods.com/article/114-api-acceptable-use-policy) reserves
personal API keys for testing and personal use. The "paste a personal key" option exists only until the application
is registered. Once Nexus issues our SSO slug, sign-in goes through SSO, and the manual option is kept or removed as
Nexus prefers. Users can see and revoke the installer's access on their Nexus **API Access** page at any time.

## API calls

All calls go to `https://api.nexusmods.com/v1/` and carry `Application-Name: Fallout 3 Access`,
`Application-Version: <installer version>` and a `User-Agent` of the form
`Fallout3Access-Installer/<version> (+https://github.com/Bruticus2131/fallout3-access)`.

| Call | When |
|---|---|
| `users/validate.json` | once, to tell the user whether the key works and whether the account is Premium |
| `games/fallout3/mods/{id}/files.json` | once per needed mod (at most two) |
| `games/fallout3/mods/{id}/files/{file_id}/download_link.json` | once per needed mod, for its main file |

That is at most five requests per installation, far below the
[rate limits](https://help.nexusmods.com/article/105-i-have-reached-a-daily-or-hourly-limit-api-requests-have-been-consumed-rate-limit-exceeded-what-does-this-mean)
(20,000 per day, then 500 per hour). Nothing runs in the background, and nothing is retried in a loop.

## What we do not do

- **We do not work around the Premium rule.** If the API refuses a download link, the user is told to download that
  file manually from the mod page.
- **We do not keep the API key.** It lives only in memory for one installation run: it is not written to the
  registry or to any settings file. The temporary file used to hand it from the sign-in step to the installer is
  deleted immediately.
- **We send the key only to the Nexus API.** It never goes to any other server; the project has no server of its own.
- **We do not scrape or rehost.** The only data fetched is the file list and the download link for the files being
  installed.
