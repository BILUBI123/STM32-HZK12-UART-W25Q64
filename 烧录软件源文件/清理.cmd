@echo off
cd /d "%~dp0"
echo ========================================
echo   自动化测试上位机 - 清理编译产物
echo ========================================
echo.
if exist bin (
    echo 删除 bin （请稍候）...
    rd /s /q bin
)
if exist obj (
    echo 删除 obj ...
    rd /s /q obj
)
if exist publish (
    echo 删除 publish ...
    rd /s /q publish
)

echo.
echo 清理完成！源码和交付包均不受影响。
echo 下次 dotnet build / dotnet publish 会自动重新生成。
echo.
exit