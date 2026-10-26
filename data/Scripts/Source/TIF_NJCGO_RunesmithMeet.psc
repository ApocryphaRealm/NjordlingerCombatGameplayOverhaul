;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 1
Scriptname TIF_NJCGO_RunesmithMeet Extends TopicInfo Hidden
; Njordlinger Combat Gameplay Overhaul (GPL-3.0-or-later) - the runesmith's quest, first talk: stage 10 and the objective.

;BEGIN FRAGMENT Fragment_0
Function Fragment_0(ObjectReference akSpeakerRef)
Actor akSpeaker = akSpeakerRef as Actor
;BEGIN CODE
GetOwningQuest().SetStage(10)
GetOwningQuest().SetObjectiveDisplayed(10)
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the end comment
