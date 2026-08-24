// NadbDaemon.java — минимальный демон-помощник, исполняемый на устройстве
// через `app_process` (под uid shell — те же права, что и обычный adb shell,
// см. обсуждение: это НЕ привилегированный доступ, а просто более быстрый
// и богатый интерфейс к Java Framework API вместо парсинга текстового
// вывода dumpsys/settings/svc).
//
// ВАЖНО про безопасность: LocalServerSocket в namespace ABSTRACT виден всем
// процессам на устройстве, включая любое установленное приложение без
// всяких permissions. Поэтому каждое соединение проверяется по uid пира:
// демон общается только с shell (2000) и root (0), т.е. с тем, кто и так мог
// бы выполнить эти действия через adb.
//
// Протокол: примитивный построчный текст.
//   клиент -> "имя_команды аргумент1 аргумент2\n"
//   демон   -> "OK <данные>\n"  или  "ERR <сообщение>\n"
//
// TODO:
//   - структурированный формат ответа (JSON) вместо простого текста,
//     когда данных станет больше одной строки
//   - команды, которые реально нужны нашему use-case (см. dispatch())
//   - снятие оверлея (wm.removeView)

import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Looper;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.lang.reflect.Method;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicBoolean;

public class NadbDaemon {

    /** uid процесса shell в Android (android.os.Process.SHELL_UID). */
    private static final int UID_SHELL = 2000;
    /** uid процесса root. */
    private static final int UID_ROOT = 0;

    /** Сколько клиентов обслуживаем одновременно. */
    private static final int MAX_CLIENTS = 4;

    /** Закрываем молчащего клиента, чтобы не держать поток вечно. */
    private static final int CLIENT_TIMEOUT_MS = 120000;

    public static void main(String[] args) throws Exception {
        String socketName = args.length > 0 ? args[0] : "nadb_daemon";

        LocalServerSocket server;
        try {
            server = new LocalServerSocket(socketName);
        } catch (IOException e) {
            // Раньше здесь просто летел stacktrace без объяснений.
            System.err.println("[nadb-daemon] не удалось занять localabstract:" + socketName
                    + " — возможно, демон уже запущен (" + e.getMessage() + ")");
            System.exit(1);
            return;
        }

        System.out.println("[nadb-daemon] listening on localabstract:" + socketName);

        // Раньше клиенты обслуживались строго по очереди прямо в accept()-цикле,
        // и один клиент, открывший сокет и ничего не пишущий, вешал весь демон
        // на readLine() — тривиальный DoS со стороны любого приложения.
        ThreadFactory factory = r -> {
            Thread t = new Thread(r, "nadb-client");
            t.setDaemon(true);
            return t;
        };
        ExecutorService pool = Executors.newFixedThreadPool(MAX_CLIENTS, factory);

        while (true) {
            final LocalSocket client;
            try {
                client = server.accept();
            } catch (IOException e) {
                System.err.println("[nadb-daemon] accept() failed: " + e);
                continue;
            }
            pool.execute(() -> handleClient(client));
        }
    }

    /**
     * Пускаем только shell и root.
     *
     * Без этой проверки любое приложение на устройстве могло подключиться
     * к localabstract:nadb_daemon и использовать его как shell-прокси, получив
     * возможности, на которые у него нет ни одного permission.
     */
    private static boolean isAuthorized(LocalSocket client) {
        try {
            Credentials creds = client.getPeerCredentials();
            if (creds == null) return false;

            int uid = creds.getUid();
            if (uid == UID_SHELL || uid == UID_ROOT) return true;

            System.err.println("[nadb-daemon] отклонён клиент uid=" + uid + " pid=" + creds.getPid());
            return false;
        } catch (IOException e) {
            // Не смогли подтвердить личность — значит не пускаем (fail-closed).
            System.err.println("[nadb-daemon] не удалось прочитать peer credentials: " + e);
            return false;
        }
    }

    private static void handleClient(LocalSocket client) {
        try {
            if (!isAuthorized(client)) {
                try (PrintWriter out = new PrintWriter(
                        new OutputStreamWriter(client.getOutputStream()), true)) {
                    out.println("ERR unauthorized");
                }
                return;
            }

            client.setSoTimeout(CLIENT_TIMEOUT_MS);

            try (BufferedReader in = new BufferedReader(
                         new InputStreamReader(client.getInputStream()));
                 PrintWriter out = new PrintWriter(
                         new OutputStreamWriter(client.getOutputStream()), true)) {

                String line;
                while ((line = in.readLine()) != null) {
                    out.println(dispatch(line.trim()));
                }
            }
        } catch (IOException e) {
            // клиент отключился или вышел таймаут — нормальная ситуация
        } catch (Throwable t) {
            // Раньше ловился только IOException, и любой RuntimeException из dispatch()
            // убивал весь демон целиком.
            System.err.println("[nadb-daemon] ошибка при обслуживании клиента: " + t);
        } finally {
            try { client.close(); } catch (IOException ignored) {}
        }
    }

    // ── Здесь живёт вся логика команд ──
    // Идея: вместо каждого запроса гонять новый shell-процесс (adb shell ...),
    // мы уже внутри долгоживущего Java-процесса на устройстве — можно
    // напрямую дёргать Java Framework API (через рефлексию для @hide-классов
    // вроде ActivityThread, или напрямую, если класс публичный).
    private static String dispatch(String cmd) {
        if (cmd.isEmpty()) return "ERR empty command";

        String[] parts = cmd.split("\\s+");
        String name = parts[0];

        try {
            switch (name) {
                case "ping":
                    return "OK pong";

                case "uptime":
                    // android.os.SystemClock — публичный API, не нужна рефлексия
                    return "OK " + android.os.SystemClock.elapsedRealtime();

                case "echo":
                    // Было cmd.substring(Math.min(cmd.length(), 5)) — для "echoFOO"
                    // это возвращало "FOO", хотя такой команды не существует.
                    return "OK " + (cmd.length() > 5 ? cmd.substring(5) : "");

                case "overlay":
                    return tryShowOverlay();

                // TODO: сюда добавлять реальные команды по мере надобности,
                // например прямой вызов CameraManager.setTorchMode() через
                // рефлексию на ActivityThread.currentApplication(), минуя
                // shell-обёртки, которых для торча просто не существует.

                default:
                    return "ERR unknown command: " + name;
            }
        } catch (Throwable e) {
            return "ERR exception: " + e;
        }
    }

    // ── ActivityThread — единственное место, где нужна рефлексия:
    // это @hide-класс, его нет в публичном android.jar, поэтому обычный
    // import + typed-вызов не скомпилируется. Всё остальное (WindowManager,
    // TextView, Looper...) — публичный API, обычные импорты.
    private static Context obtainSystemContext() throws Exception {
        Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
        Method systemMain = activityThreadClass.getMethod("systemMain");
        Object activityThread = systemMain.invoke(null);
        Method getSystemContext = activityThreadClass.getMethod("getSystemContext");
        return (Context) getSystemContext.invoke(activityThread);
    }

    // Результат последней попытки overlay — пишется из отдельного потока,
    // читается из dispatch() после короткого ожидания (см. ниже).
    private static volatile String lastOverlayResult = null;

    // Looper.prepareMainLooper() можно вызвать в процессе только один раз —
    // второй вызов "overlay" раньше гарантированно падал с IllegalStateException
    // и плодил лишние потоки.
    private static final AtomicBoolean overlayStarted = new AtomicBoolean(false);

    // TODO: убрать оверлей (wm.removeView) — сейчас только для проверки,
    //       что вообще можно нарисовать поверх экрана; нет команды снятия.
    private static String tryShowOverlay() {
        if (!overlayStarted.compareAndSet(false, true)) {
            return lastOverlayResult != null
                    ? lastOverlayResult
                    : "OK overlay already started";
        }

        lastOverlayResult = null;

        Thread t = new Thread(() -> {
            try {
                // Нужен свой Looper — тот, что держит accept()-цикл в main(),
                // для UI/WindowManager не подходит. И важно: именно
                // prepareMainLooper(), а не просто prepare() — часть кода
                // WindowManager/ViewRootImpl внутри обращается к статическому
                // Looper.getMainLooper() (process-wide), а не к луперу
                // текущего потока. Раз наш настоящий main() (accept()-цикл)
                // луперы вообще не создаёт, process-wide "main looper" ничей —
                // регистрируем этот поток как главный.
                if (Looper.getMainLooper() == null) {
                    Looper.prepareMainLooper();
                }

                Context ctx = obtainSystemContext();
                WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);

                TextView tv = new TextView(ctx);
                tv.setText("nadb overlay test");
                tv.setTextColor(Color.RED);
                tv.setBackgroundColor(Color.BLACK);
                tv.setPadding(24, 24, 24, 24);

                // TYPE_APPLICATION_OVERLAY — с Android 8.0 (API 26).
                // На более старых версий нужен другой TYPE_* и другая
                // permission-модель — тут сознательно не поддержано.
                WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.WRAP_CONTENT,
                        WindowManager.LayoutParams.WRAP_CONTENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
                params.gravity = Gravity.TOP | Gravity.START;
                params.x = 50;
                params.y = 150;

                wm.addView(tv, params);

                // Если addView() не бросил исключение синхронно — окно добавлено.
                lastOverlayResult = "OK view added, entering loop";
                Looper.loop();
            } catch (Throwable e) {
                // Сюда прилетит, например, SecurityException, если
                // permission-барьер на этом устройстве/версии не пропускает
                // shell/system uid для TYPE_APPLICATION_OVERLAY.
                lastOverlayResult = "ERR " + e;
                overlayStarted.set(false); // позволяем повторить попытку
            }
        }, "nadb-overlay-thread");
        t.setDaemon(true);
        t.start();

        // addView() бросает исключение синхронно (если бросает вообще) —
        // ждём немного, чтобы успеть поймать именно эту ошибку, а не
        // рапортовать "OK" раньше времени.
        for (int i = 0; i < 20 && lastOverlayResult == null; i++) {
            try { Thread.sleep(50); } catch (InterruptedException ignored) {}
        }
        return lastOverlayResult != null ? lastOverlayResult : "OK started (async, no immediate error)";
    }
}
