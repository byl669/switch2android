package dev.ot2.port;

import android.content.Context;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * 首启把随包的中文替换字体铺到 {@code <files>/games/<title>/shared-font.otf}。
 *
 * native/shared_font.cpp 的 {@code font_candidates()} 先看游戏目录里这个文件，找不到才
 * 退到 /system/fonts 下的 Roboto —— 那些只有拉丁字形，中文全部落到 notdef，就是满屏的
 * "?"。这个文件原先只在设备上手工推过，卸载重装即失，所以必须跟着 APK 一起发。
 *
 * 幂等：{@code .shared-font} 里存版本串，与 {@link #VERSION} 一致就跳过。覆盖写也安全：
 * 先把整块写进临时文件再 rename，中途断电不会留下半个字体。
 */
final class SharedFont {
    // 换字体文件就要改这个串，否则老设备不会重新铺。
    static final String VERSION = "f1";

    private static final String ASSET = "shared-font.otf";
    private static final String NAME = "shared-font.otf";
    private static final String STAMP = ".shared-font";

    static void install(Context context, File dataRoot, BundleInstaller.Progress progress) throws Exception {
        File stamp = new File(dataRoot, STAMP);
        if (upToDate(stamp)) return;
        InputStream probe;
        try {
            probe = context.getAssets().open(ASSET);
        } catch (IOException missing) {
            // 没随包发布（开发期热推流程）时什么都不做，保持原有行为。
            return;
        }
        probe.close();
        if (progress != null) progress.update("正在安装中文字体…");
        File target = new File(dataRoot, NAME);
        File temporary = new File(dataRoot, NAME + ".tmp");
        long copied = 0;
        try (InputStream raw = new BufferedInputStream(context.getAssets().open(ASSET), 256 * 1024);
             FileOutputStream out = new FileOutputStream(temporary)) {
            byte[] buffer = new byte[256 * 1024];
            int read;
            while ((read = raw.read(buffer)) > 0) {
                out.write(buffer, 0, read);
                copied += read;
            }
            out.getFD().sync();
        }
        if (copied < 64 * 1024) throw new IOException("字体资源不完整: " + copied);
        if (!temporary.renameTo(target)) {
            temporary.delete();
            throw new IOException("字体无法落盘");
        }
        try (FileOutputStream out = new FileOutputStream(stamp)) {
            out.write(VERSION.getBytes(StandardCharsets.US_ASCII));
            out.getFD().sync();
        }
    }

    private static boolean upToDate(File stamp) {
        if (!stamp.isFile()) return false;
        try (InputStream input = new FileInputStream(stamp)) {
            return new String(input.readNBytes(64), StandardCharsets.US_ASCII).equals(VERSION);
        } catch (IOException unreadable) {
            return false;
        }
    }
}
