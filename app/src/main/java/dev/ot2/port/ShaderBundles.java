package dev.ot2.port;

import android.content.Context;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * 首次运行把离线导出的 d2r 着色器包铺到 {@code <files>/games/<title>/shaders/vk}。
 *
 * d2r 引擎只在那个目录里找 {@code <key>.xgs}（bundle_directory，见 d2r_bridge.cpp），
 * 而 game.s2a 里带的是离线导出器的 .spv/.res.json 参考件，运行时一个都不读 —— 那些包
 * 是开发期手工推上去的。少了任何一个，缺到 compute dispatch 时会直接
 * "Dispatch has no genuine compute shader" 自杀，所以它们必须跟着 APK 一起发。
 *
 * 幂等：{@code shaders/.bundles} 里存版本串，与 {@link #VERSION} 一致就跳过。卸载重装
 * 会连这个目录一起删，因此这里也是唯一的重建点。
 */
final class ShaderBundles {
    // 包的集合一变就要改这个串，否则老设备不会重新铺。
    // v2：v1 的包来自 s2a-xgs ∪ xgs-fix，其中 150 个是旧版本，覆盖后人物丢失。
    static final String VERSION = "v2-1965";

    private static final String ASSET = "shader-bundles.zip";
    private static final String STAMP = ".bundles";

    static void install(Context context, File dataRoot, BundleInstaller.Progress progress) throws Exception {
        File shaderRoot = new File(dataRoot, "shaders");
        File target = new File(shaderRoot, "vk");
        File stamp = new File(shaderRoot, STAMP);
        if (upToDate(stamp)) return;

        try (InputStream probe = context.getAssets().open(ASSET)) {
            probe.close();
        } catch (IOException missing) {
            // 没有随包发布（开发期热推流程）时什么都不做，保持原有行为。
            return;
        }

        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("着色器目录不可用");
        byte[] buffer = new byte[1024 * 1024];
        int count = 0;
        try (InputStream raw = context.getAssets().open(ASSET);
             ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw, 1024 * 1024))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) continue;
                String name = entry.getName();
                // 只接受引擎的命名规则：内容哈希 16 位十六进制 + .xgs。这条同时挡住了
                // 任何带路径分隔符或大小写变体的条目。
                if (!name.matches("[0-9a-f]{16}\\.xgs")) throw new IOException("着色器包名非法: " + name);
                try (FileOutputStream out = new FileOutputStream(new File(target, name))) {
                    int read;
                    while ((read = zip.read(buffer)) > 0) out.write(buffer, 0, read);
                }
                if (++count % 250 == 0 && progress != null) progress.update("正在安装着色器：" + count);
            }
        }
        if (count == 0) throw new IOException("着色器包为空");
        if (progress != null) progress.update("正在安装着色器：" + count);
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
