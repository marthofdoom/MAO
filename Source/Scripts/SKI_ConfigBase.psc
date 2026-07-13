; Minimal compilation stub for SKI_ConfigBase.
; At runtime this class is provided by SkyUI's BSA.
; Only declares the subset of the API used by MRO_MCM.psc.
Scriptname SKI_ConfigBase extends SKI_QuestBase Hidden

String[] Property Pages Auto
String Property ModName Auto

Int Property OPTION_FLAG_NONE
    Int Function Get()
        Return 0
    EndFunction
EndProperty

Int Property OPTION_FLAG_DISABLED
    Int Function Get()
        Return 1
    EndFunction
EndProperty

Int Property TOP_TO_BOTTOM
    Int Function Get()
        Return 2
    EndFunction
EndProperty

Int Property LEFT_TO_RIGHT
    Int Function Get()
        Return 1
    EndFunction
EndProperty

Event OnConfigInit()
EndEvent

Event OnPageReset(String a_page)
EndEvent

Event OnOptionSelect(Int a_option)
EndEvent

Event OnOptionSliderOpen(Int a_option)
EndEvent

Event OnOptionSliderAccept(Int a_option, Float a_value)
EndEvent

Event OnOptionHighlight(Int a_option)
EndEvent

Int Function AddHeaderOption(String a_text, Int a_flags = 0) native
Int Function AddTextOption(String a_text, String a_value, Int a_flags = 0) native
Int Function AddToggleOption(String a_text, Bool a_checked, Int a_flags = 0) native
Int Function AddSliderOption(String a_text, Float a_value, String a_format = "{0}", Int a_flags = 0) native
Int Function AddEmptyOption() native
Function SetCursorFillMode(Int a_fillMode) native
Function SetCursorPosition(Int a_position) native
Function SetToggleOptionValue(Int a_option, Bool a_value) native
Function SetSliderOptionValue(Int a_option, Float a_value, String a_format = "{0}") native
Function SetSliderDialogStartValue(Float a_value) native
Function SetSliderDialogDefaultValue(Float a_value) native
Function SetSliderDialogRange(Float a_min, Float a_max) native
Function SetSliderDialogInterval(Float a_interval) native
Function ForcePageReset() native
Function SetInfoText(String a_text) native
