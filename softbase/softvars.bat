@echo off
SET SOFTBASE=%~dp0
SET BSON_LIBRARY_PATH=%SOFTBASE%\lib
SET MONGO_LIBRARY_PATH=%SOFTBASE%\lib
SET PATH=%SOFTBASE%\bin;%SOFTBASE%\bin\plugins;%SOFTBASE%\lib;%PATH%