# Locale based metric/imperial system

Group `locale-units` (func_replace `sp_is_metric` at `CLocalisation::Metric`, 0x56d220). Setting
`silentpatch.units` (locale, metric, imperial) replaces the INI's `Units`.

**Bug.** The game picks units from the menu language, so an English-language player always gets imperial units.

**Reproduction.** Open the Misc stats page; the game calls `CLocalisation::Metric`.

**Result.** Passed (M7): imperial for this Mac's US region by default, metric with
`--set silentpatch.units=metric`.
