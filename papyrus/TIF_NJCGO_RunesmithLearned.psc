;BEGIN FRAGMENT CODE - Do not edit anything between this and the end comment
;NEXT FRAGMENT INDEX 1
Scriptname TIF_NJCGO_RunesmithLearned Extends TopicInfo Hidden
; Njordlinger Combat Gameplay Overhaul (GPL-3.0-or-later) - the runesmith's quest, a word of power learned: stage 100.

;BEGIN FRAGMENT Fragment_0
Function Fragment_0(ObjectReference akSpeakerRef)
Actor akSpeaker = akSpeakerRef as Actor
;BEGIN CODE
GetOwningQuest().SetObjectiveDisplayed(10)
GetOwningQuest().SetObjectiveCompleted(10)
GetOwningQuest().SetStage(100)
;END CODE
EndFunction
;END FRAGMENT

;END FRAGMENT CODE - Do not edit anything between this and the end comment
