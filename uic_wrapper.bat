@echo off
SetLocal EnableDelayedExpansion
(set PATH=C:\deps\qt570\qtbase\lib;!PATH!)
if defined QT_PLUGIN_PATH (
    set QT_PLUGIN_PATH=C:\deps\qt570\qtbase\plugins;!QT_PLUGIN_PATH!
) else (
    set QT_PLUGIN_PATH=C:\deps\qt570\qtbase\plugins
)
C:\deps\qt570\qtbase\bin\uic.exe %*
EndLocal
