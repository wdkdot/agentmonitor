!macro NSIS_HOOK_PREUNINSTALL
  ; The autostart plugin owns only these two per-user values. Remove both so
  ; uninstall never leaves a dead startup entry behind.
  DeleteRegValue HKCU "SOFTWARE\Microsoft\Windows\CurrentVersion\Run" "AI Usage Display"
  DeleteRegValue HKCU "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "AI Usage Display"
!macroend
