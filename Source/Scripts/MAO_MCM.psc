Scriptname MAO_MCM extends MCM_ConfigBase
; Marth's Alchemy Overhaul — MCM registration shim.
;
; Deliberately empty. MCM Helper renders every control from
; Data/MCM/Config/MAO/config.json and persists each ModSetting to
; Data/MCM/Settings/MAO.ini. The MAO SKSE plugin reads that INI at load
; and re-reads it live whenever a menu closes, so no Papyrus logic lives
; here. This subclass exists only so SkyUI/MCM Helper have a concrete
; MCM_ConfigBase-derived quest script to register — the proven pattern
; copied from MEO_MCM (MEO is the template).
