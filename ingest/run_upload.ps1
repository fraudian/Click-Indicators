# Push mirrored blobs to R2, then emit the D1 index SQL.
# Resumable: sync_d1.py only uploads rows still in state 'mirrored' and flips them to 'uploaded',
# so re-running after an interruption picks up where it stopped rather than starting over.
$env:PYTHONIOENCODING = "utf-8"
Set-Location C:\Users\lndsm\ClickGuide\ingest
& "C:\Users\lndsm\AppData\Local\Programs\Python\Python312\python.exe" -u sync_d1.py --upload
