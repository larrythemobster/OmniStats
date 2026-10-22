# Display and reminders

## Cards

Open **Settings > Display** and select **In-game overlay** or **Second-monitor dashboard**. Each mode has its own card toggles. Enabling an unplaced card adds it automatically. Turning a card off preserves its position and visibility rule.

Use **Position cards** at the top of the card list to move, resize, or dock cards. Docked cards show a title bar while positioning; drag it off the group to undock that card, or onto another card to move it there. **Done positioning** returns to normal play. Layout editing does not contain visibility controls.

Each enabled overlay card has its own **Show when** selector in the card list:

- **Always**: visible in menus and matches.
- **While key held**: visible while holding the overlay key or controller button.
- **In matches**: visible throughout an active match.
- **After an event**: visible for 1 to 30 seconds after the selected match event. The event and duration appear below the card.
- **In menus**: visible outside matches.

Docked cards keep their own rules. **Show card options** adds goal-replay hiding for each card, the key-held override for In matches, In menus, and After an event, and the session metrics, playlist columns, and other card details.

Existing placements and visibility rules are preserved. New standalone cards enabled from settings start with **Always** visibility.

## Rank progress and MMR graph

Under **Settings > Accounts & services > Show rank options**, **Rank progress** has its own **Follow current playlist** switch. It does not change the player-rank or MMR-graph playlist selections. Turn it off to choose a **Default rank playlist**. Existing configurations retain their previous rank-progress playlist and follow preference.

## Tournaments

The tournament countdown has the same compact layout in both modes: next tournament name, region, and time remaining. Enable it under **Settings > Display**. It does not show a schedule list, second-chance row, or update timestamp.

Configure region, reminder timing, the standard-only filter, and dashboard sound under **Settings > Reminders**. Reminders work independently of the countdown card. They require an OmniStats account, as does the countdown.

Auto region reads Rocket League's cached datacenter pings, falling back to recent match reservations in `Launch.log`. Manual selection is available because the game's tournament-region preference can differ from its lowest-ping region.

The standard-only reminder filter uses the API's `bUpdateSkill` flag. It does not filter the countdown. Overlay reminders last five seconds outside matches and three seconds during matches. Dashboard mode supports an optional sound cue once per slot. Reminders do not take keyboard focus.

Schedules refresh every 30 minutes. Failed refreshes retry after five minutes while retaining the last successful schedule for that region. Signing out disables schedule access and reminders.

## Settings navigation

- **Display**: mode, card visibility, placement, and card details.
- **Reminders**: tournament notification preferences and region.
- **Appearance**: text size and units; fonts and colors under **Show fonts and colors**.
- **Controls**: keyboard and controller bindings.
- **Accounts & services**: OmniStats sign-in, integrations, Rocket League identity, and rank details.
- **Data & support**: history, data tools, sessions, startup preferences, and troubleshooting.
