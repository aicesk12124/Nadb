# nadb_daemon — PoC демона на устройстве (app_process)

## Что это
Минимальный демон под uid `shell` (те же права, что и adb shell — не рут,
не привилегированный доступ), слушающий localabstract-сокет и принимающий
построчные текстовые команды. Цель — иметь долгоживущий Java-процесс на
устройстве с прямым доступом к Framework API, вместо парсинга текстового
вывода dumpsys/settings на каждый запрос.

## Сборка (Windows)
1. Убедитесь, что в Android Studio установлены (File -> Settings ->
   Languages & Frameworks -> Android SDK -> SDK Tools):
   - Android SDK Build-Tools
   - любая SDK Platform (например android-34) — нужна только для
     компиляции (android.jar), на устройство не пакуется
2. Запустите `build_daemon.bat` из этой папки — он сам найдёт нужные
   версии в вашем `C:\Users\shara\AppData\Local\Android\Sdk`.
3. Результат: `nadb_daemon.jar`

## Запуск на устройстве
```
adb push nadb_daemon.jar /data/local/tmp/nadb_daemon.jar
adb shell app_process -cp /data/local/tmp/nadb_daemon.jar /data/local/tmp NadbDaemon
```
(оставить это в отдельном окне — процесс работает в foreground)

## Проверка с хоста
В другом окне:
```
adb forward tcp:9999 localabstract:nadb_daemon
```
Дальше любым способом отправить строку на 127.0.0.1:9999 (telnet, nc,
или просто PowerShell):
```powershell
$c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 9999)
$s = $c.GetStream()
$w = New-Object System.IO.StreamWriter($s)
$r = New-Object System.IO.StreamReader($s)
$w.AutoFlush = $true
$w.WriteLine("ping")
$r.ReadLine()   # ожидаем "OK pong"
```

## Дальше (не реализовано, TODO)
- Добавить `forward:` сервис в AdbBridge (аналогично уже реализованным
  host:/device: сервисам) — чтобы push+forward+общение с демоном шло
  через уже написанный raw-socket мост, а не руками через adb.exe
- Реальные команды в dispatch() под ваши задачи (например, попытка
  вызвать CameraManager.setTorchMode() через рефлексию для флешлайта —
  см. обсуждение permission-барьера, работоспособность нужно проверять
  эмпирически на конкретном устройстве)
- Параллельная обработка клиентов, JSON-протокол, авто-рестарт демона
  при обрыве (аналогично ensure_server_running() в AdbBridge)
