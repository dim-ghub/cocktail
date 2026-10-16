# Cocktail

Unified CLI, native Roblox account manager, and live Vulkan FPS limiter for **Nightcap** and **Sober** on Linux.

---

## Features

- **Native Roblox Account Switcher Manager**: Inspects and manages the native Roblox account switcher (`PreviousAccountsList`, `AccountBlob`, and `AccountSwitcherMetadataMap` in `appStorage.json`). Easily list accounts, switch active accounts, import accounts from Sober, Chrome, Brave, Firefox, and CiderHQ, and launch directly into any account.
- **Rejoin & Server Hop**: Jump back into the last server you were in (from Nightcap or Sober logs) using whichever client you choose, or hop to a different non-full server of the same game.
- **Live Vulkan FPS Limiter**: Dynamically adjust the frame rate cap on the fly while Roblox is running without restarting the game. Works for both native Nightcap and Flatpak Sober via the Vulkan layer (`VK_LAYER_SOBER_fpslimit`).

---

## Installation

```bash
# Build and install Cocktail CLI and native Vulkan layer (for Nightcap & host)
make install-native

# Or full install (including Flatpak VulkanLayer extension for Sober)
make install
```

This installs `cocktail` (and backwards-compatible `soberfps`) to `~/.local/bin/`.

---

## Account Management (Native Account Switcher & Cookie Vault)

Cocktail interfaces directly with Roblox's native account switcher and includes automated session discovery:

```bash
# List all accounts in the native switcher and cached vault
cocktail account list

# Auto-discover and import session cookies from Brave, Chrome, Firefox, Sober, and Nightcap
cocktail account import

# Manually store/paste a .ROBLOSECURITY session cookie for an account
cocktail account set-cookie <username> [cookie]

# Sync newly authenticated sessions from active instances
cocktail account sync

# Switch the default active account for next launch
cocktail account switch <username>

# Launch Nightcap directly into a specific account
cocktail account launch <username>
```

---

## Live FPS Limiter

Change frame rate caps on the fly (via `VK_LAYER_SOBER_fpslimit`):

```bash
cocktail fps              # show current cap and in-game caps
cocktail fps 144          # cap at 144 fps
cocktail fps off          # remove the cap (unlimited)
```

### Patterns (Ctrl-C restores previous cap):
```bash
cocktail fps random 144            # random cap 1..144, new one every second
cocktail fps random 144 30 -e 0.5-3  # 30..144, random 0.5–3 s delay
cocktail fps seq 1 12 123          # step through caps
cocktail fps seq 30 60 --once      # step through once, keep last cap
cocktail fps ramp 10 120 5         # ramp up and down
cocktail fps wave 30 144 20        # smooth sine wave between 30 and 144
```

`soberfps` remains fully functional as a direct alias for `cocktail fps`.

### Disabling the Vulkan Layer
If you want to launch Nightcap without the Vulkan FPS layer:
```bash
# Using Cocktail CLI flag
cocktail launch --no-fps
cocktail account launch <username> --no-fps

# Or using environment variables
DISABLE_COCKTAIL_FPSLIMIT=1 cocktail launch
VK_LOADER_LAYERS_DISABLE=VK_LAYER_SOBER_fpslimit mocktail
```

---

## Rejoin Last Server / Server Hop

Cocktail reads the most recent `Joining game` entry from Nightcap and Sober logs, then relaunches straight into that server:

```bash
# Rejoin the last server with whichever client last joined it
cocktail rejoin

# Rejoin with a specific client (e.g. left a Sober server, rejoin it in Nightcap)
cocktail rejoin nightcap
cocktail rejoin sober

# Only consider the last server joined in Sober's logs
cocktail rejoin nightcap --from sober

# Server hop: random non-full server of the same game, skipping ones visited in the last 10 minutes
cocktail rejoin --hop

# Show the server and launch URI without launching anything
cocktail rejoin --dry-run
```

The chosen client is stopped first if it's already running, then relaunched into the server. Use `-a/--account` to rejoin Nightcap with a specific account.

---

## Uninstallation

```bash
make uninstall
```
