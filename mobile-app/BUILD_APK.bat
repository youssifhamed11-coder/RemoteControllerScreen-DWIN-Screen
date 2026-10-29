@echo off
title Building Screen Control APK...
cd /d "%~dp0"
if exist "C:\Program Files\Java\jdk-17.0.12\bin\java.exe" (
  set "JAVA_HOME=C:\Program Files\Java\jdk-17.0.12"
) else (
  set "JAVA_HOME=C:\Program Files\Android\Android Studio2\jbr"
)
echo Building... (first time can take a few minutes)
echo START %date% %time% > build_log.txt
call gradlew.bat assembleRelease --no-daemon --stacktrace >> build_log.txt 2>&1
if exist "app\build\outputs\apk\release\app-release.apk" (
  copy /y "app\build\outputs\apk\release\app-release.apk" "ScreenControl-v1.4.0.apk" >nul
  echo BUILD_OK >> build_log.txt
  echo.
  echo DONE - ScreenControl-v1.4.0.apk is ready
) else (
  echo BUILD_FAILED >> build_log.txt
  echo.
  echo BUILD FAILED - see build_log.txt
)
timeout /t 8
