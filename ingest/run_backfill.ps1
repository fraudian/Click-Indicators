# Full backfill launcher.
#
# Exists because the Telegram credentials live in the USER environment (setx), which a shell
# started before they were set does not inherit - and because `python` on this machine resolves to
# an agent venv without telethon. Both are easy to get wrong by hand and silently fatal.
$env:TG_API_ID   = [Environment]::GetEnvironmentVariable("TG_API_ID", "User")
$env:TG_API_HASH = [Environment]::GetEnvironmentVariable("TG_API_HASH", "User")
$env:PYTHONIOENCODING = "utf-8"
Set-Location C:\Users\lndsm\ClickGuide\ingest
& "C:\Users\lndsm\AppData\Local\Programs\Python\Python312\python.exe" -u mirror_telegram.py
