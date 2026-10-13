# Overlay visibility rules

In overlay edit mode (**Settings > Cards > Edit overlay layout**), each overlay container has its own **Visibility** rule:

- **While scoreboard key held**: shows the container while holding the overlay key or controller button.
- **Always during match**: keeps the container visible throughout an active match.
- **For N s after event**: shows the container for 1 to 30 seconds after match start, the first kickoff countdown, each kickoff countdown, a goal, match end, or the podium screen.
- **In menus only**: shows the container only outside matches.
- **Also while key held** and **Hide during goal replays** toggles per container.

New installs default the Lobby Ranks container to **8 s after the first kickoff countdown** (plus while the key is held), the post-match summary to **20 s after the podium**, and all other containers to **While scoreboard key held**. Existing configs keep their previous behavior on upgrade unless changed in edit mode.
